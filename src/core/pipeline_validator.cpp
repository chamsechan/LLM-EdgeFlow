#include "core/pipeline_validator.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_config.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

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
    case DiagnosticCode::kUnknownBiz:
      return "UNKNOWN_BIZ";
    case DiagnosticCode::kUnknownNodeType:
      return "UNKNOWN_NODE_TYPE";
    case DiagnosticCode::kUnknownModelType:
      return "UNKNOWN_MODEL_TYPE";
    case DiagnosticCode::kUnknownBackend:
      return "UNKNOWN_BACKEND";
    case DiagnosticCode::kBackendProtocolMismatch:
      return "BACKEND_PROTOCOL_MISMATCH";
    case DiagnosticCode::kUnknownModelConfigField:
      return "UNKNOWN_MODEL_CONFIG_FIELD";
    case DiagnosticCode::kUnknownBackendConfigField:
      return "UNKNOWN_BACKEND_CONFIG_FIELD";
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
    case DiagnosticCode::kNodeBizMismatch:
      return "NODE_BIZ_MISMATCH";
    case DiagnosticCode::kMissingInputProducer:
      return "MISSING_INPUT_PRODUCER";
    case DiagnosticCode::kDuplicatePortProducer:
      return "DUPLICATE_PORT_PRODUCER";
    case DiagnosticCode::kMissingBizOutput:
      return "MISSING_BIZ_OUTPUT";
    case DiagnosticCode::kNodeNotParallelSafe:
      return "NODE_NOT_PARALLEL_SAFE";
    case DiagnosticCode::kParallelWriteConflict:
      return "PARALLEL_WRITE_CONFLICT";
    case DiagnosticCode::kSerializedModelConcurrency:
      return "SERIALIZED_MODEL_CONCURRENCY";
    case DiagnosticCode::kPortCardinalityMismatch:
      return "PORT_CARDINALITY_MISMATCH";
    case DiagnosticCode::kPortProvenanceMismatch:
      return "PORT_PROVENANCE_MISMATCH";
    case DiagnosticCode::kPortLifetimeMismatch:
      return "PORT_LIFETIME_MISMATCH";
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
    case PipelineErrorCode::kUnknownModelType:
      return DiagnosticCode::kUnknownModelType;
    case PipelineErrorCode::kUnknownBackend:
      return DiagnosticCode::kUnknownBackend;
    case PipelineErrorCode::kInvalidDependency:
      return DiagnosticCode::kInvalidDependency;
    case PipelineErrorCode::kDagCycle:
      return DiagnosticCode::kDagCycle;
    case PipelineErrorCode::kRegistryConflict:
      return DiagnosticCode::kRegistryConflict;
    case PipelineErrorCode::kModelMaterializationFailed:
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

PortDefinition EffectivePortDefinition(const PortDefinition& declared,
                                       const NodeDefinition& node_definition,
                                       const nlohmann::json& node_config) {
  PortDefinition effective = declared;
  if (declared.lifetime_config_field.empty()) return effective;

  const auto& field_name = declared.lifetime_config_field;
  if (node_config.contains(field_name) && node_config[field_name].is_string()) {
    effective.lifetime = node_config[field_name].get<std::string>();
    return effective;
  }
  auto field =
      std::find_if(node_definition.config_fields.begin(),
                   node_definition.config_fields.end(),
                   [&](const auto& item) { return item.name == field_name; });
  if (field != node_definition.config_fields.end() &&
      field->default_value.is_string()) {
    effective.lifetime = field->default_value.get<std::string>();
  }
  return effective;
}

bool CardinalityCompatible(const std::string& producer,
                           const std::string& consumer) {
  if (producer == consumer || producer == "N:M" || consumer == "N:M") {
    return true;
  }
  return consumer == "N:1";
}

bool ProvenanceCompatible(const std::string& producer,
                          const std::string& consumer) {
  if (consumer == "preserve" || consumer == "aggregate") return true;
  return producer == consumer;
}

int LifetimeRank(const std::string& lifetime) {
  if (lifetime == "request") return 0;
  if (lifetime == "session") return 1;
  if (lifetime == "global") return 2;
  return -1;
}

bool TraversesParent(const std::filesystem::path& path) {
  const std::string normalized = path.string();
  return normalized == ".." || normalized.rfind("../", 0) == 0 ||
         normalized.rfind("..\\", 0) == 0;
}

bool LifetimeCompatible(const std::string& producer,
                        const std::string& consumer) {
  return LifetimeRank(producer) >= LifetimeRank(consumer);
}

void ValidatePortFlowContract(const PortDefinition& producer,
                              const PortDefinition& consumer,
                              const ParsedNodeConfig& node,
                              const std::string& logical_port,
                              const std::string& producer_id,
                              ValidationReport* report) {
  const std::string path = "/pipeline/" + std::to_string(node.source_index) +
                           "/ports/inputs/" + logical_port;
  if (!CardinalityCompatible(producer.cardinality, consumer.cardinality)) {
    Add(report, DiagnosticCode::kPortCardinalityMismatch, path,
        "Port cardinality mismatch: producer '" + producer.cardinality +
            "' cannot feed consumer '" + consumer.cardinality + "'",
        node.id, logical_port, {producer_id});
  }
  if (!ProvenanceCompatible(producer.provenance_policy,
                            consumer.provenance_policy)) {
    Add(report, DiagnosticCode::kPortProvenanceMismatch, path,
        "Port provenance mismatch: producer policy '" +
            producer.provenance_policy + "' cannot satisfy consumer policy '" +
            consumer.provenance_policy + "'",
        node.id, logical_port, {producer_id});
  }
  if (!LifetimeCompatible(producer.lifetime, consumer.lifetime)) {
    Add(report, DiagnosticCode::kPortLifetimeMismatch, path,
        "Port lifetime mismatch: producer lifetime '" + producer.lifetime +
            "' is shorter than consumer lifetime '" + consumer.lifetime + "'",
        node.id, logical_port, {producer_id});
  }
}

}  // namespace

bool ValidateAndNormalizeConfig(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input, nlohmann::json* normalized,
    std::vector<ValidationDiagnostic>* diagnostics,
    const std::string& base_pointer, DiagnosticCode unknown_field_code) {
  if (!input.is_object()) {
    if (diagnostics) {
      ValidationDiagnostic diag;
      diag.code = DiagnosticCode::kConfigFieldType;
      diag.path = base_pointer;
      diag.message = "Config must be a JSON object";
      diagnostics->push_back(std::move(diag));
    }
    return false;
  }

  bool ok = true;
  const auto reject = [&](DiagnosticCode code, std::string path,
                          std::string message) {
    ok = false;
    if (diagnostics) {
      diagnostics->push_back(
          {code, std::move(path), std::move(message), "error", {}, {}, {}, {}});
    }
  };
  nlohmann::json result = nlohmann::json::object();

  // 1. 未知字段校验与 suggestions 生成
  for (auto it = input.begin(); it != input.end(); ++it) {
    const std::string& key = it.key();
    bool found = std::any_of(schema.begin(), schema.end(),
                             [&](const auto& f) { return f.name == key; });
    if (!found) {
      ok = false;
      if (diagnostics) {
        ValidationDiagnostic diag;
        diag.code = unknown_field_code;
        diag.path =
            base_pointer.empty() ? ("/" + key) : (base_pointer + "/" + key);
        diag.message = "Unknown config field: " + key;
        for (const auto& field : schema) {
          diag.suggestions.push_back(field.name);
        }
        diagnostics->push_back(std::move(diag));
      }
    }
  }

  // 2. 已声明字段约束校验与默认值注入
  for (const auto& field : schema) {
    std::string field_path = base_pointer.empty()
                                 ? ("/" + field.name)
                                 : (base_pointer + "/" + field.name);
    if (!input.contains(field.name)) {
      if (field.required) {
        reject(DiagnosticCode::kMissingConfigField, field_path,
               "Missing required config field: " + field.name);
      } else if (!field.default_value.is_null()) {
        result[field.name] = field.default_value;
      }
      continue;
    }

    const auto& val = input[field.name];
    if (!MatchesKind(val, field.kind)) {
      reject(DiagnosticCode::kConfigFieldType, field_path,
             "Expected " + std::string(ConfigValueKindName(field.kind)));
      continue;
    }

    if (val.is_number()) {
      double num_val = val.get<double>();
      if (field.minimum.has_value() && num_val < *field.minimum) {
        reject(
            DiagnosticCode::kConfigFieldRange, field_path,
            "Numeric value is below minimum " + std::to_string(*field.minimum));
      } else if (field.maximum.has_value() && num_val > *field.maximum) {
        reject(
            DiagnosticCode::kConfigFieldRange, field_path,
            "Numeric value exceeds maximum " + std::to_string(*field.maximum));
      }
    }

    if (field.kind == ConfigValueKind::kString && !field.enum_values.empty() &&
        val.is_string()) {
      std::string str_val = val.get<std::string>();
      if (std::find(field.enum_values.begin(), field.enum_values.end(),
                    str_val) == field.enum_values.end()) {
        reject(DiagnosticCode::kConfigFieldEnum, field_path,
               "String value '" + str_val + "' not in allowed enum values");
      }
    }

    result[field.name] = val;
  }

  if (ok && normalized) {
    *normalized = std::move(result);
  }
  return ok;
}

namespace {

nlohmann::json ValidateConfigFields(
    const std::vector<ConfigFieldDefinition>& definitions,
    const nlohmann::json& config, const std::string& path_prefix,
    const std::string& subject_id, ValidationReport* report) {
  std::vector<ValidationDiagnostic> diags;
  nlohmann::json normalized = nlohmann::json::object();
  if (config.is_object()) {
    for (const auto& field : definitions) {
      if (config.contains(field.name)) {
        normalized[field.name] = config[field.name];
      } else if (!field.default_value.is_null()) {
        normalized[field.name] = field.default_value;
      }
    }
  }

  nlohmann::json validated;
  if (ValidateAndNormalizeConfig(definitions, config, &validated, &diags,
                                 path_prefix)) {
    normalized = std::move(validated);
  }
  for (auto& d : diags) {
    d.node_id = subject_id;
    report->diagnostics.push_back(std::move(d));
  }
  return normalized;
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

  const auto* business = PipelineCatalog::FindBiz(parsed.biz_name);
  if (!business && policy == ValidationPolicy::kStrict) {
    Add(&report, DiagnosticCode::kUnknownBiz, "/biz_name",
        "No registered biz contract accepts pipeline name: " + parsed.biz_name);
  }
  if (NodeFactory::Instance().HasConflict()) {
    Add(&report, DiagnosticCode::kRegistryConflict, "/pipeline",
        "Node registry contains registration conflicts");
  }
  if (ModelRegistry::Instance().HasConflict()) {
    Add(&report, DiagnosticCode::kRegistryConflict, "/models",
        "Model registry contains registration conflicts");
  }
  if (BackendRegistry::Instance().HasConflict()) {
    Add(&report, DiagnosticCode::kRegistryConflict, "/models",
        "Backend registry contains registration conflicts");
  }

  std::unordered_map<std::string, std::string> model_capabilities;
  std::unordered_map<std::string, InferenceConcurrency> model_concurrency;
  for (const auto& model : parsed.models) {
    auto model_def_opt = ModelRegistry::Instance().Find(model.model_type);
    bool has_model = ModelRegistry::Instance().Has(model.model_type);
    if (!has_model ||
        (!model_def_opt.has_value() && policy == ValidationPolicy::kStrict)) {
      Add(&report, DiagnosticCode::kUnknownModelType,
          "/models/" + std::to_string(model.source_index) + "/model_type",
          "Unknown model_type: " + model.model_type);
    }

    auto backend_def_opt = BackendRegistry::Instance().Find(model.backend);
    bool has_backend = BackendRegistry::Instance().Has(model.backend);
    if (!has_backend ||
        (!backend_def_opt.has_value() && policy == ValidationPolicy::kStrict)) {
      Add(&report, DiagnosticCode::kUnknownBackend,
          "/models/" + std::to_string(model.source_index) + "/backend",
          "Unknown backend: " + model.backend);
    }

    if (model_def_opt && backend_def_opt) {
      if (model.capability != model_def_opt->capability) {
        Add(&report, DiagnosticCode::kModelCapabilityMismatch,
            "/models/" + std::to_string(model.source_index) + "/capability",
            "Model capability mismatch: declared '" + model.capability +
                "' but ModelDefinition specifies '" +
                model_def_opt->capability + "'");
      }

      const auto& supported_protocols = backend_def_opt->supported_protocols;
      bool protocol_supported =
          std::find(supported_protocols.begin(), supported_protocols.end(),
                    model_def_opt->required_protocol) !=
          supported_protocols.end();
      if (!protocol_supported) {
        Add(&report, DiagnosticCode::kBackendProtocolMismatch,
            "/models/" + std::to_string(model.source_index) + "/backend",
            "Backend '" + model.backend +
                "' does not support required protocol '" +
                std::string(
                    ExecutionProtocolName(model_def_opt->required_protocol)) +
                "' for model '" + model.model_type + "'");
      }

      nlohmann::json normalized_mcfg = nlohmann::json::object();
      std::vector<ValidationDiagnostic> mcfg_diags;
      ValidateAndNormalizeConfig(
          model_def_opt->config_fields, model.model_config, &normalized_mcfg,
          &mcfg_diags,
          "/models/" + std::to_string(model.source_index) + "/model_config",
          DiagnosticCode::kUnknownModelConfigField);
      for (auto& d : mcfg_diags) {
        report.diagnostics.push_back(std::move(d));
      }

      nlohmann::json normalized_bcfg = nlohmann::json::object();
      std::vector<ValidationDiagnostic> bcfg_diags;
      ValidateAndNormalizeConfig(
          backend_def_opt->config_fields, model.backend_config,
          &normalized_bcfg, &bcfg_diags,
          "/models/" + std::to_string(model.source_index) + "/backend_config",
          DiagnosticCode::kUnknownBackendConfigField);
      for (auto& d : bcfg_diags) {
        report.diagnostics.push_back(std::move(d));
      }

      // 8. Layer 2 only performs environment-neutral lexical path checks.
      // Deployment roots are resolved by Layer 1 before runtime validation.
      const auto normalized_path =
          std::filesystem::path(model.model_path).lexically_normal();
      if (!normalized_path.is_absolute() && TraversesParent(normalized_path)) {
        Add(&report, DiagnosticCode::kFieldRange,
            "/models/" + std::to_string(model.source_index) + "/model_path",
            "Model path cannot traverse outside model root directory: " +
                model.model_path);
      }
      InferenceConcurrency effective_concurrency =
          (model_def_opt->concurrency == InferenceConcurrency::kSerialized ||
           backend_def_opt->concurrency == InferenceConcurrency::kSerialized)
              ? InferenceConcurrency::kSerialized
              : InferenceConcurrency::kConcurrent;

      model_capabilities[model.model_id] = model.capability;
      model_concurrency[model.model_id] = effective_concurrency;

      ValidatedModelPlan model_plan;
      model_plan.model_id = model.model_id;
      model_plan.capability = model.capability;
      model_plan.model_type = model.model_type;
      model_plan.backend = model.backend;
      model_plan.resolved_model_path = normalized_path.string();
      model_plan.normalized_model_config = std::move(normalized_mcfg);
      model_plan.normalized_backend_config = std::move(normalized_bcfg);
      model_plan.protocol = model_def_opt->required_protocol;
      model_plan.effective_concurrency = effective_concurrency;
      model_plan.source_index = model.source_index;
      plan.models.push_back(std::move(model_plan));
    }
  }

  const auto& nodes = parsed.nodes;
  std::unordered_map<std::string, const ParsedNodeConfig*> node_by_id;
  std::unordered_map<std::string, const NodeDefinition*> def_by_id;
  std::unordered_map<std::string, nlohmann::json> normalized_config_by_node;
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

      if (business && !definition->biz_names.empty() &&
          std::find(definition->biz_names.begin(), definition->biz_names.end(),
                    parsed.biz_name) == definition->biz_names.end()) {
        Add(&report, DiagnosticCode::kNodeBizMismatch,
            "/pipeline/" + std::to_string(node.source_index) + "/node_type",
            "Node type is not declared for biz: " + parsed.biz_name, node.id);
      }

      auto normalized_config = ValidateConfigFields(
          definition->config_fields, node.config,
          "/pipeline/" + std::to_string(node.source_index) + "/config", node.id,
          &report);
      normalized_config_by_node[node.id] = normalized_config;

      if (!definition->model_capability.empty()) {
        std::string model_id;
        if (normalized_config.contains(definition->model_config_field) &&
            normalized_config[definition->model_config_field].is_string()) {
          model_id = normalized_config[definition->model_config_field]
                         .get<std::string>();
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
    const auto normalized_it = normalized_config_by_node.find(id);
    const nlohmann::json& normalized_config =
        normalized_it == normalized_config_by_node.end()
            ? node.config
            : normalized_it->second;

    ValidatedNodePlan node_plan;
    node_plan.node = node;
    node_plan.normalized_config = normalized_config;

    // 校验未声明的输入端口映射
    for (const auto& entry : node.ports.inputs) {
      const auto& in_port = entry.first;
      bool declared =
          std::any_of(definition.inputs.begin(), definition.inputs.end(),
                      [&](const auto& item) { return item.key == in_port; });
      if (!declared) {
        Add(&report, DiagnosticCode::kUnknownField,
            "/pipeline/" + std::to_string(node.source_index) +
                "/ports/inputs/" + in_port,
            "Unknown logical input port '" + in_port + "' for node type '" +
                definition.node_type + "'",
            id, in_port);
      }
    }

    // 校验未声明的输出端口映射
    for (const auto& entry : node.ports.outputs) {
      const auto& out_port = entry.first;
      bool declared =
          std::any_of(definition.outputs.begin(), definition.outputs.end(),
                      [&](const auto& item) { return item.key == out_port; });
      if (!declared) {
        Add(&report, DiagnosticCode::kUnknownField,
            "/pipeline/" + std::to_string(node.source_index) +
                "/ports/outputs/" + out_port,
            "Unknown logical output port '" + out_port + "' for node type '" +
                definition.node_type + "'",
            id, out_port);
      }
    }

    std::unordered_set<std::string> bound_input_ports;
    for (const auto& declared_input : definition.inputs) {
      const auto input = EffectivePortDefinition(declared_input, definition,
                                                 normalized_config);
      std::string actual_key = input.key;
      bool explicitly_bound = false;
      auto port_it = node.ports.inputs.find(input.key);
      if (port_it != node.ports.inputs.end()) {
        actual_key = port_it->second;
        explicitly_bound = true;
        bound_input_ports.insert(input.key);
      } else if (input.required) {
        bound_input_ports.insert(input.key);
      }

      node_plan.ports.push_back({input.key, actual_key, input.type_id,
                                 input.cardinality, input.provenance_policy,
                                 input.lifetime, PortDirection::kInput});

      if (!input.required && !explicitly_bound) continue;
      bool found = false;
      auto producer_it = producers.find(actual_key);
      if (producer_it != producers.end()) {
        // Blackboard keys use last-writer semantics. Resolve the nearest
        // topologically preceding ancestor, including its type, instead of
        // accepting an older producer that is shadowed at runtime.
        for (auto it = producer_it->second.rbegin();
             it != producer_it->second.rend(); ++it) {
          if (is_ancestor(it->first, id)) {
            if (it->second.type_id == input.type_id) {
              found = true;
              ValidatePortFlowContract(it->second, input, node, input.key,
                                       it->first, &report);
            }
            break;
          }
        }
      }
      if (!found && producer_it == producers.end()) {
        auto root_port = ingress.find(actual_key);
        if (root_port != ingress.end() &&
            root_port->second.type_id == input.type_id) {
          found = true;
          ValidatePortFlowContract(root_port->second, input, node, input.key,
                                   "$ingress", &report);
        }
      }
      if (!found && business) {
        std::vector<std::string> suggestions;
        for (const auto& candidate : PipelineCatalog::Nodes()) {
          if (std::any_of(candidate.outputs.begin(), candidate.outputs.end(),
                          [&](const auto& output) {
                            return output.type_id == input.type_id;
                          })) {
            suggestions.push_back(candidate.node_type);
          }
        }
        Add(&report, DiagnosticCode::kMissingInputProducer,
            explicitly_bound
                ? ("/pipeline/" + std::to_string(node.source_index) +
                   "/ports/inputs/" + input.key)
                : ("/pipeline/" + std::to_string(node.source_index)),
            "No business ingress or ancestor node produces port '" + input.key +
                "' (bound key: '" + actual_key + "') of type '" +
                input.type_id + "'",
            id, input.key, {}, suggestions);
      }
    }

    // 校验端口组合约束 (Port Group Constraints)
    for (const auto& constraint : definition.port_constraints) {
      bool satisfied = true;
      if (constraint.kind == PortConstraintKind::kExactOneGroupOf) {
        int fully_matched_groups = 0;
        for (size_t g = 0; g < constraint.port_groups.size(); ++g) {
          const auto& group = constraint.port_groups[g];
          bool all_in = true;
          for (const auto& p : group) {
            if (!bound_input_ports.count(p)) {
              all_in = false;
              break;
            }
          }
          if (all_in) {
            bool only_this_group = true;
            for (const auto& p : bound_input_ports) {
              if (std::find(group.begin(), group.end(), p) == group.end()) {
                for (size_t og = 0; og < constraint.port_groups.size(); ++og) {
                  if (og != g) {
                    if (std::find(constraint.port_groups[og].begin(),
                                  constraint.port_groups[og].end(),
                                  p) != constraint.port_groups[og].end()) {
                      only_this_group = false;
                      break;
                    }
                  }
                }
              }
              if (!only_this_group) break;
            }
            if (only_this_group) {
              fully_matched_groups++;
            }
          }
        }
        satisfied = (fully_matched_groups == 1);
      } else {
        size_t count = 0;
        for (const auto& p : constraint.ports) {
          if (bound_input_ports.count(p)) {
            count++;
          }
        }
        switch (constraint.kind) {
          case PortConstraintKind::kAtLeastOneOf:
            satisfied = (count >= 1);
            break;
          case PortConstraintKind::kExactlyOneOf:
            satisfied = (count == 1);
            break;
          case PortConstraintKind::kAllOrNone:
            satisfied = (count == 0 || count == constraint.ports.size());
            break;
          case PortConstraintKind::kAtMostOneOf:
            satisfied = (count <= 1);
            break;
          case PortConstraintKind::kExactOneGroupOf:
            break;
        }
      }
      if (!satisfied) {
        std::string msg = constraint.message.empty()
                              ? ("Port constraint violation for node '" +
                                 definition.node_type + "'")
                              : constraint.message;
        Add(&report, DiagnosticCode::kInvalidCombination,
            "/pipeline/" + std::to_string(node.source_index), msg, id);
      }
    }

    for (const auto& declared_output : definition.outputs) {
      const auto output = EffectivePortDefinition(declared_output, definition,
                                                  normalized_config);
      std::string actual_key = output.key;
      auto port_it = node.ports.outputs.find(output.key);
      if (port_it != node.ports.outputs.end()) {
        actual_key = port_it->second;
      }

      node_plan.ports.push_back({output.key, actual_key, output.type_id,
                                 output.cardinality, output.provenance_policy,
                                 output.lifetime, PortDirection::kOutput});

      auto& existing = producers[actual_key];
      if (!existing.empty()) {
        Add(&report, DiagnosticCode::kDuplicatePortProducer,
            "/pipeline/" + std::to_string(node.source_index),
            "Write-once Blackboard port is produced more than once: " +
                actual_key,
            id, output.key, {existing.back().first});
      }
      PortDefinition resolved_output = output;
      resolved_output.key = actual_key;
      existing.push_back({id, std::move(resolved_output)});
    }

    plan.node_plans[id] = std::move(node_plan);
  }

  if (business) {
    for (const auto& required : business->egress) {
      bool found = false;
      auto it = producers.find(required.key);
      if (it != producers.end() && !it->second.empty()) {
        found = it->second.back().second.type_id == required.type_id;
      }
      if (!found) {
        Add(&report, DiagnosticCode::kMissingBizOutput, "/pipeline",
            "Pipeline does not produce required biz output: " + required.key,
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
          auto concurrency = model_concurrency.find(model_id->second);
          if (concurrency != model_concurrency.end() &&
              concurrency->second == InferenceConcurrency::kSerialized) {
            auto inserted =
                serialized_model_users.emplace(model_id->second, id);
            if (!inserted.second) {
              const auto& node = *node_by_id.at(id);
              Add(&report, DiagnosticCode::kSerializedModelConcurrency,
                  "/pipeline/" + std::to_string(node.source_index) +
                      "/config/" + def_it->second->model_config_field,
                  "Parallel layer shares serialized model instance: " +
                      model_id->second,
                  id, {}, {inserted.first->second});
            }
          }
        }
        const auto& node_plan = plan.node_plans[id];
        for (const auto& p : node_plan.ports) {
          if (p.direction != PortDirection::kOutput) continue;
          auto inserted = writes.emplace(p.blackboard_key, id);
          if (!inserted.second) {
            Add(&report, DiagnosticCode::kParallelWriteConflict, "/pipeline",
                "Parallel layer writes the same port: " + p.blackboard_key, id,
                p.logical_name, {inserted.first->second});
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

}  // namespace llm_edgeflow
