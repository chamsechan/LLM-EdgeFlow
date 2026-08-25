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
namespace {

std::string PipelineErrorCodeName(PipelineErrorCode code) {
  switch (code) {
    case PipelineErrorCode::kOk:
      return "OK";
    case PipelineErrorCode::kJsonParse:
      return "JSON_PARSE";
    case PipelineErrorCode::kConfigFileOpen:
      return "CONFIG_FILE_OPEN";
    case PipelineErrorCode::kRootType:
      return "ROOT_TYPE";
    case PipelineErrorCode::kUnknownField:
      return "UNKNOWN_FIELD";
    case PipelineErrorCode::kMissingField:
      return "MISSING_FIELD";
    case PipelineErrorCode::kFieldType:
      return "FIELD_TYPE";
    case PipelineErrorCode::kFieldRange:
      return "FIELD_RANGE";
    case PipelineErrorCode::kInvalidCombination:
      return "INVALID_COMBINATION";
    case PipelineErrorCode::kDuplicateModelId:
      return "DUPLICATE_MODEL_ID";
    case PipelineErrorCode::kDuplicateNodeId:
      return "DUPLICATE_NODE_ID";
    case PipelineErrorCode::kUnknownNodeType:
      return "UNKNOWN_NODE_TYPE";
    case PipelineErrorCode::kUnknownEngineType:
      return "UNKNOWN_ENGINE_TYPE";
    case PipelineErrorCode::kInvalidDependency:
      return "INVALID_DEPENDENCY";
    case PipelineErrorCode::kDagCycle:
      return "DAG_CYCLE";
    case PipelineErrorCode::kRegistryConflict:
      return "REGISTRY_CONFLICT";
    case PipelineErrorCode::kEngineCreateFailed:
      return "ENGINE_CREATE_FAILED";
    case PipelineErrorCode::kEngineLoadFailed:
      return "ENGINE_LOAD_FAILED";
    case PipelineErrorCode::kNodeCreateFailed:
      return "NODE_CREATE_FAILED";
    case PipelineErrorCode::kNodeInitFailed:
      return "NODE_INIT_FAILED";
    case PipelineErrorCode::kInternalException:
      return "INTERNAL_EXCEPTION";
    case PipelineErrorCode::kInvalidBuildState:
      return "INVALID_BUILD_STATE";
  }
  return "UNKNOWN";
}

void Add(ValidationReport* report, std::string code, std::string path,
         std::string message, std::string node_id = {}, std::string port = {},
         std::vector<std::string> related = {},
         std::vector<std::string> suggestions = {}) {
  report->diagnostics.push_back({std::move(code), std::move(path),
                                 std::move(message), "error",
                                 std::move(node_id), std::move(port),
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
        Add(report, "DAG_CYCLE", path, "Node cannot depend on itself", node.id,
            {}, {dep});
        invalid_reference = true;
      } else if (index.find(dep) == index.end()) {
        Add(report, "INVALID_DEPENDENCY", path,
            "Dependency references an unknown node: " + dep, node.id, {},
            {dep});
        invalid_reference = true;
      } else if (!seen.insert(dep).second) {
        Add(report, "DUPLICATE_DEPENDENCY", path,
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
    Add(report, "DAG_CYCLE", "/pipeline",
        "Cyclic dependency detected in pipeline");
    return false;
  }
  return !invalid_reference;
}

}  // namespace

nlohmann::json ValidationReport::ToJson() const {
  nlohmann::json items = nlohmann::json::array();
  for (const auto& diagnostic : diagnostics) {
    nlohmann::json item = {{"code", diagnostic.code},
                           {"path", diagnostic.path},
                           {"message", diagnostic.message},
                           {"severity", diagnostic.severity}};
    if (!diagnostic.node_id.empty()) item["node_id"] = diagnostic.node_id;
    if (!diagnostic.port.empty()) item["port"] = diagnostic.port;
    if (!diagnostic.related_nodes.empty())
      item["related_nodes"] = diagnostic.related_nodes;
    if (!diagnostic.suggestions.empty())
      item["suggestions"] = diagnostic.suggestions;
    items.push_back(std::move(item));
  }
  return {{"schema_version", 1},
          {"ok", ok},
          {"diagnostics", std::move(items)},
          {"plan",
           {{"topological_order", topological_order},
            {"layers", topological_layers}}}};
}

ValidationReport PipelineValidator::Validate(const nlohmann::json& root) {
  ValidationReport report;
  ParsedPipelineConfig parsed;
  PipelineDiagnostic parse_diag;
  if (!ParsePipelineConfig(root, &parsed, &parse_diag)) {
    Add(&report, PipelineErrorCodeName(parse_diag.code), parse_diag.path,
        parse_diag.message);
    return report;
  }

  const auto* business = PipelineCatalog::FindBusiness(parsed.business_name);
  if (!business) {
    Add(&report, "UNKNOWN_BUSINESS", "/business_name",
        "No registered business contract accepts pipeline name: " +
            parsed.business_name);
  }
  if (NodeFactory::Instance().HasConflict()) {
    Add(&report, "REGISTRY_CONFLICT", "/pipeline",
        "Node registry contains registration conflicts");
  }
  if (EngineFactory::Instance().HasConflict()) {
    Add(&report, "REGISTRY_CONFLICT", "/models",
        "Engine registry contains registration conflicts");
  }

  std::unordered_map<std::string, std::string> model_capabilities;
  for (const auto& model : parsed.models) {
    const auto* engine = PipelineCatalog::FindEngine(model.engine_type);
    if (!EngineFactory::Instance().Has(model.engine_type) || !engine) {
      Add(&report, "UNKNOWN_ENGINE_TYPE",
          "/models/" + std::to_string(model.source_index) + "/engine_type",
          "Unknown engine_type: " + model.engine_type);
      continue;
    }
    model_capabilities[model.model_id] = engine->capability;
    std::unordered_map<std::string, const ConfigFieldDefinition*> fields;
    for (const auto& field : engine->config_fields) fields[field.name] = &field;
    for (auto it = model.config.begin(); it != model.config.end(); ++it) {
      auto found = fields.find(it.key());
      std::string path = "/models/" + std::to_string(model.source_index) +
                         "/config/" + it.key();
      if (found == fields.end()) {
        Add(&report, "UNKNOWN_CONFIG_FIELD", path,
            "Unknown engine config field: " + it.key());
      } else if (!MatchesKind(it.value(), found->second->kind)) {
        Add(&report, "CONFIG_FIELD_TYPE", path,
            "Expected " +
                std::string(ConfigValueKindName(found->second->kind)));
      }
    }
  }

  auto nodes = EffectiveNodes(parsed);
  std::unordered_map<std::string, const ParsedNodeConfig*> node_by_id;
  std::unordered_map<std::string, const NodeDefinition*> def_by_id;
  for (const auto& node : nodes) {
    node_by_id[node.id] = &node;
    const auto* definition = PipelineCatalog::FindNode(node.node_type);
    if (!NodeFactory::Instance().Has(node.node_type) || !definition) {
      Add(&report, "UNKNOWN_NODE_TYPE",
          "/pipeline/" + std::to_string(node.source_index) + "/node_type",
          "Unknown node_type or missing catalog definition: " + node.node_type,
          node.id);
      continue;
    }
    def_by_id[node.id] = definition;

    std::unordered_map<std::string, const ConfigFieldDefinition*> fields;
    for (const auto& field : definition->config_fields)
      fields[field.name] = &field;
    for (auto it = node.config.begin(); it != node.config.end(); ++it) {
      std::string path = "/pipeline/" + std::to_string(node.source_index) +
                         "/config/" + it.key();
      auto found = fields.find(it.key());
      if (found == fields.end()) {
        Add(&report, "UNKNOWN_CONFIG_FIELD", path,
            "Unknown node config field: " + it.key(), node.id);
        continue;
      }
      const auto& field = *found->second;
      if (!MatchesKind(it.value(), field.kind)) {
        Add(&report, "CONFIG_FIELD_TYPE", path,
            "Expected " + std::string(ConfigValueKindName(field.kind)),
            node.id);
        continue;
      }
      if (it.value().is_number()) {
        double value = it.value().get<double>();
        if ((field.minimum && value < *field.minimum) ||
            (field.maximum && value > *field.maximum)) {
          Add(&report, "CONFIG_FIELD_RANGE", path,
              "Numeric value is outside the supported range", node.id);
        }
      }
    }

    if (!definition->model_capability.empty()) {
      std::string model_id;
      if (node.config.contains(definition->model_config_field) &&
          node.config[definition->model_config_field].is_string()) {
        model_id =
            node.config[definition->model_config_field].get<std::string>();
      } else {
        auto field = std::find_if(
            definition->config_fields.begin(), definition->config_fields.end(),
            [&](const auto& item) {
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
      std::string path = "/pipeline/" + std::to_string(node.source_index) +
                         "/config/" + definition->model_config_field;
      if (capability == model_capabilities.end()) {
        Add(&report, "UNKNOWN_MODEL_REFERENCE", path,
            "Node references an unknown model_id: " + model_id, node.id);
      } else if (capability->second != definition->model_capability) {
        Add(&report, "MODEL_CAPABILITY_MISMATCH", path,
            "Node requires model capability '" + definition->model_capability +
                "' but model provides '" + capability->second + "'",
            node.id);
      }
    }
  }

  const size_t pre_topology_errors = report.diagnostics.size();
  ResolveTopology(nodes, &report);
  if (report.diagnostics.size() != pre_topology_errors || !business ||
      report.topological_order.size() != nodes.size()) {
    report.ok = report.diagnostics.empty();
    return report;
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
  for (const auto& port : business->ingress) ingress[port.key] = port;
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
      if (!found) {
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
        Add(&report, "MISSING_INPUT_PRODUCER",
            "/pipeline/" + std::to_string(node.source_index),
            "No business ingress or ancestor node produces required port '" +
                input.key + "' of type '" + input.type_id + "'",
            id, input.key, {}, suggestions);
      }
    }
    for (const auto& output : definition.outputs) {
      auto& existing = producers[output.key];
      if (!existing.empty() && !output.allow_override) {
        Add(&report, "DUPLICATE_PORT_PRODUCER",
            "/pipeline/" + std::to_string(node.source_index),
            "Port is produced more than once without override permission: " +
                output.key,
            id, output.key, {existing.back().first});
      }
      existing.push_back({id, output});
    }
  }

  for (const auto& required : business->egress) {
    bool found = false;
    auto it = producers.find(required.key);
    if (it != producers.end()) {
      found = std::any_of(it->second.begin(), it->second.end(),
                          [&](const auto& producer) {
                            return producer.second.type_id == required.type_id;
                          });
    }
    if (!found) {
      Add(&report, "MISSING_BUSINESS_OUTPUT", "/pipeline",
          "Pipeline does not produce required business output: " + required.key,
          {}, required.key);
    }
  }

  if (parsed.execution_mode == "parallel") {
    for (const auto& layer : report.topological_layers) {
      std::unordered_map<std::string, std::string> writes;
      for (const auto& id : layer) {
        auto def_it = def_by_id.find(id);
        if (def_it == def_by_id.end()) continue;
        if (!def_it->second->parallel_safe && layer.size() > 1) {
          Add(&report, "NODE_NOT_PARALLEL_SAFE", "/pipeline",
              "Node is not declared safe for wavefront parallel execution", id);
        }
        for (const auto& output : def_it->second->outputs) {
          auto inserted = writes.emplace(output.key, id);
          if (!inserted.second) {
            Add(&report, "PARALLEL_WRITE_CONFLICT", "/pipeline",
                "Parallel layer writes the same port: " + output.key, id,
                output.key, {inserted.first->second});
          }
        }
      }
    }
  }

  report.ok = report.diagnostics.empty();
  return report;
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
      diagnostic->code = PipelineErrorCodeName(parse_diag.code);
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
      diagnostic->code = PipelineErrorCodeName(parse_diag.code);
      diagnostic->path = parse_diag.path;
      diagnostic->message = parse_diag.message;
    }
    return false;
  }

  *output = std::move(normalized);
  return true;
}

}  // namespace alg_framework
