#include "core/pipeline_validator.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "contracts/config_schema_validation.h"
#include "contracts/path_utils.h"
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
                                 std::move(related), std::move(suggestions),
                                 std::nullopt});
}

NodePortDefinition EffectivePortDefinition(
    const NodePortDefinition& declared, const NodeDefinition& node_definition,
    const nlohmann::json& node_config) {
  NodePortDefinition effective = declared;
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
  // Retain the portable leading Windows-parent check even on hosts whose
  // native filesystem treats backslashes as ordinary filename characters.
  return HasParentPathComponent(path) || path.string().rfind("..\\", 0) == 0;
}

bool LifetimeCompatible(const std::string& producer,
                        const std::string& consumer) {
  return LifetimeRank(producer) >= LifetimeRank(consumer);
}

void ValidatePortFlowContract(const PortContract& producer,
                              const PortContract& consumer,
                              const std::string& path,
                              const std::string& node_id,
                              const std::string& logical_port,
                              const std::string& producer_id,
                              ValidationReport* report) {
  if (!CardinalityCompatible(producer.cardinality, consumer.cardinality)) {
    Add(report, DiagnosticCode::kPortCardinalityMismatch, path,
        "Port cardinality mismatch: producer '" + producer.cardinality +
            "' cannot feed consumer '" + consumer.cardinality + "'",
        node_id, logical_port, {producer_id});
  }
  if (!ProvenanceCompatible(producer.provenance_policy,
                            consumer.provenance_policy)) {
    Add(report, DiagnosticCode::kPortProvenanceMismatch, path,
        "Port provenance mismatch: producer policy '" +
            producer.provenance_policy + "' cannot satisfy consumer policy '" +
            consumer.provenance_policy + "'",
        node_id, logical_port, {producer_id});
  }
  if (!LifetimeCompatible(producer.lifetime, consumer.lifetime)) {
    Add(report, DiagnosticCode::kPortLifetimeMismatch, path,
        "Port lifetime mismatch: producer lifetime '" + producer.lifetime +
            "' is shorter than consumer lifetime '" + consumer.lifetime + "'",
        node_id, logical_port, {producer_id});
  }
}

}  // namespace

bool ValidateAndNormalizeConfig(
    const std::vector<ConfigFieldDefinition>& schema,
    const nlohmann::json& input, nlohmann::json* normalized,
    std::vector<ValidationDiagnostic>* diagnostics,
    const std::string& base_pointer, DiagnosticCode unknown_field_code) {
  std::vector<ConfigFieldValidationError> field_errors;
  bool ok =
      ValidateAndNormalizeFields(schema, input, normalized, &field_errors);
  if (!ok && diagnostics) {
    for (const auto& err : field_errors) {
      ValidationDiagnostic diag;
      diag.path = err.kind == ConfigFieldErrorKind::kNotAnObject
                      ? base_pointer
                      : base_pointer + "/" + err.field_name;
      diag.message = err.message;
      switch (err.kind) {
        case ConfigFieldErrorKind::kNotAnObject:
          diag.code = DiagnosticCode::kConfigFieldType;
          break;
        case ConfigFieldErrorKind::kUnknownField:
          diag.code = unknown_field_code;
          for (const auto& field : schema) {
            diag.suggestions.push_back(field.name);
          }
          break;
        case ConfigFieldErrorKind::kMissingField:
          diag.code = DiagnosticCode::kMissingConfigField;
          break;
        case ConfigFieldErrorKind::kTypeMismatch:
          diag.code = DiagnosticCode::kConfigFieldType;
          break;
        case ConfigFieldErrorKind::kOutOfRange:
        case ConfigFieldErrorKind::kNonFinite:
          diag.code = DiagnosticCode::kConfigFieldRange;
          break;
        case ConfigFieldErrorKind::kInvalidEnum:
          diag.code = DiagnosticCode::kConfigFieldEnum;
          break;
      }
      diag.severity = "error";
      diagnostics->push_back(std::move(diag));
    }
  }
  return ok;
}

namespace {

nlohmann::json ValidateConfigFields(
    const std::vector<ConfigFieldDefinition>& definitions,
    const nlohmann::json& config, const std::string& path_prefix,
    const std::string& subject_id, ValidationReport* report,
    bool* out_valid = nullptr) {
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
  bool ok = ValidateAndNormalizeConfig(definitions, config, &validated, &diags,
                                       path_prefix);
  if (ok) {
    normalized = std::move(validated);
  }
  if (out_valid) *out_valid = ok;
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

size_t LevenshteinDistance(std::string_view s1, std::string_view s2) {
  const size_t m = s1.size();
  const size_t n = s2.size();
  std::vector<size_t> dp(n + 1);
  for (size_t j = 0; j <= n; ++j) dp[j] = j;
  for (size_t i = 1; i <= m; ++i) {
    size_t prev = dp[0];
    dp[0] = i;
    for (size_t j = 1; j <= n; ++j) {
      size_t temp = dp[j];
      if (s1[i - 1] == s2[j - 1]) {
        dp[j] = prev;
      } else {
        dp[j] = 1 + std::min({prev, dp[j], dp[j - 1]});
      }
      prev = temp;
    }
  }
  return dp[n];
}

std::string EscapeJsonPointer(std::string_view token) {
  std::string escaped;
  escaped.reserve(token.size());
  for (char c : token) {
    if (c == '~') {
      escaped += "~0";
    } else if (c == '/') {
      escaped += "~1";
    } else {
      escaped += c;
    }
  }
  return escaped;
}

bool ValueMatchesConfigKind(const nlohmann::json& value, ConfigValueKind kind) {
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

struct DiagnosticIdentity {
  DiagnosticCode code;
  std::string node_id;
  std::string port;
  std::string subpath;

  bool operator==(const DiagnosticIdentity& other) const {
    return code == other.code && node_id == other.node_id &&
           port == other.port && subpath == other.subpath;
  }
};

DiagnosticIdentity GetDiagnosticIdentity(const ValidationDiagnostic& d) {
  DiagnosticIdentity id;
  id.code = d.code;
  id.node_id = d.node_id;
  id.port = d.port;
  if (d.code == DiagnosticCode::kDuplicateDependency) {
    if (!d.related_nodes.empty()) {
      id.subpath = "/depends_on/" + d.related_nodes.front();
    } else if (d.remediation.has_value() &&
               d.remediation->facts.contains("dependency_id")) {
      id.subpath = "/depends_on/" +
                   d.remediation->facts["dependency_id"].get<std::string>();
    } else {
      id.subpath = "/depends_on";
    }
  } else if (d.path.rfind("/pipeline/", 0) == 0) {
    size_t second_slash = d.path.find('/', 10);
    if (second_slash != std::string::npos) {
      id.subpath = d.path.substr(second_slash);
    } else {
      id.subpath = "/pipeline";
    }
  } else {
    id.subpath = d.path;
  }
  return id;
}

void PopulateBasicRemediation(
    ValidationDiagnostic* diag, const nlohmann::json& root,
    const PipelineCatalogSnapshot& catalog,
    [[maybe_unused]] const std::vector<ValidationDiagnostic>& all_diagnostics) {
  if (!diag || diag->remediation.has_value()) return;
  if (!root.is_object()) return;

  if (diag->code == DiagnosticCode::kUnknownConfigField) {
    if (diag->path.rfind("/pipeline/", 0) == 0 && root.contains("pipeline") &&
        root["pipeline"].is_array()) {
      size_t idx_end = diag->path.find('/', 10);
      if (idx_end != std::string::npos) {
        size_t p_idx = 0;
        try {
          p_idx = std::stoul(diag->path.substr(10, idx_end - 10));
        } catch (...) {
          p_idx = static_cast<size_t>(-1);
        }
        if (p_idx < root["pipeline"].size()) {
          const auto& node_obj = root["pipeline"][p_idx];
          std::string node_type = node_obj.value("node_type", "");
          const auto* def = catalog.FindNode(node_type);
          std::string prefix =
              "/pipeline/" + std::to_string(p_idx) + "/config/";
          if (def && diag->path.rfind(prefix, 0) == 0) {
            std::string field_name = diag->path.substr(prefix.size());
            ValidationRemediation rem;
            rem.schema_version = 1;
            rem.cause = "unknown_config_field";
            rem.summary = "节点 '" + diag->node_id + "' 的配置包含未知字段 '" +
                          field_name + "'。";
            rem.facts["field"] = field_name;

            std::vector<std::pair<size_t, std::string>> ranked;
            for (const auto& field : def->config_fields) {
              ranked.emplace_back(LevenshteinDistance(field_name, field.name),
                                  field.name);
            }
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto& a, const auto& b) {
                        if (a.first != b.first) return a.first < b.first;
                        return a.second < b.second;
                      });
            std::vector<std::string> candidate_fields;
            for (const auto& item : ranked) {
              candidate_fields.push_back(item.second);
            }
            rem.facts["candidate_fields"] = candidate_fields;
            diag->remediation = std::move(rem);
          }
        }
      }
    }
  } else if (diag->code == DiagnosticCode::kMissingConfigField) {
    if (diag->path.rfind("/pipeline/", 0) == 0 && root.contains("pipeline") &&
        root["pipeline"].is_array()) {
      size_t idx_end = diag->path.find('/', 10);
      if (idx_end != std::string::npos) {
        size_t p_idx = 0;
        try {
          p_idx = std::stoul(diag->path.substr(10, idx_end - 10));
        } catch (...) {
          p_idx = static_cast<size_t>(-1);
        }
        if (p_idx < root["pipeline"].size()) {
          const auto& node_obj = root["pipeline"][p_idx];
          std::string node_type = node_obj.value("node_type", "");
          const auto* def = catalog.FindNode(node_type);
          std::string prefix =
              "/pipeline/" + std::to_string(p_idx) + "/config/";
          std::string field_name;
          if (diag->path.rfind(prefix, 0) == 0) {
            field_name = diag->path.substr(prefix.size());
          }
          if (def && !field_name.empty()) {
            ValidationRemediation rem;
            rem.schema_version = 1;
            rem.cause = "missing_config_field";
            rem.summary = "节点 '" + diag->node_id + "' 缺少必填配置字段 '" +
                          field_name + "'。";
            rem.facts["field"] = field_name;
            auto cf_it = std::find_if(
                def->config_fields.begin(), def->config_fields.end(),
                [&](const auto& f) { return f.name == field_name; });
            if (cf_it != def->config_fields.end()) {
              rem.facts["expected_type"] = ConfigValueKindName(cf_it->kind);
              if (cf_it->minimum.has_value())
                rem.facts["minimum"] = *cf_it->minimum;
              if (cf_it->maximum.has_value())
                rem.facts["maximum"] = *cf_it->maximum;
              if (!cf_it->enum_values.empty())
                rem.facts["enum"] = cf_it->enum_values;
            }
            diag->remediation = std::move(rem);
          }
        }
      }
    }
  } else if (diag->code == DiagnosticCode::kConfigFieldType ||
             diag->code == DiagnosticCode::kConfigFieldRange ||
             diag->code == DiagnosticCode::kConfigFieldEnum) {
    if (diag->path.rfind("/pipeline/", 0) == 0 && root.contains("pipeline") &&
        root["pipeline"].is_array()) {
      size_t idx_end = diag->path.find('/', 10);
      if (idx_end != std::string::npos) {
        size_t p_idx = 0;
        try {
          p_idx = std::stoul(diag->path.substr(10, idx_end - 10));
        } catch (...) {
          p_idx = static_cast<size_t>(-1);
        }
        if (p_idx < root["pipeline"].size()) {
          const auto& node_obj = root["pipeline"][p_idx];
          std::string node_type = node_obj.value("node_type", "");
          const auto* def = catalog.FindNode(node_type);
          std::string prefix =
              "/pipeline/" + std::to_string(p_idx) + "/config/";
          std::string field_name;
          if (diag->path.rfind(prefix, 0) == 0) {
            field_name = diag->path.substr(prefix.size());
          }
          if (def && !field_name.empty()) {
            ValidationRemediation rem;
            rem.schema_version = 1;
            rem.cause = "invalid_config_value";
            rem.summary = "节点 '" + diag->node_id + "' 的配置项 '" +
                          field_name + "' 值不合法。";
            rem.facts["field"] = field_name;
            auto cf_it = std::find_if(
                def->config_fields.begin(), def->config_fields.end(),
                [&](const auto& f) { return f.name == field_name; });
            if (cf_it != def->config_fields.end()) {
              rem.facts["expected_type"] = ConfigValueKindName(cf_it->kind);
              if (cf_it->minimum.has_value())
                rem.facts["minimum"] = *cf_it->minimum;
              if (cf_it->maximum.has_value())
                rem.facts["maximum"] = *cf_it->maximum;
              if (!cf_it->enum_values.empty())
                rem.facts["enum"] = cf_it->enum_values;
            }
            diag->remediation = std::move(rem);
          }
        }
      }
    }
  } else if (diag->code == DiagnosticCode::kUnknownModelReference ||
             diag->code == DiagnosticCode::kModelCapabilityMismatch) {
    if (diag->path.rfind("/pipeline/", 0) == 0 && root.contains("pipeline") &&
        root["pipeline"].is_array()) {
      size_t idx_end = diag->path.find('/', 10);
      if (idx_end != std::string::npos) {
        size_t p_idx = 0;
        try {
          p_idx = std::stoul(diag->path.substr(10, idx_end - 10));
        } catch (...) {
          p_idx = static_cast<size_t>(-1);
        }
        if (p_idx < root["pipeline"].size()) {
          const auto& node_obj = root["pipeline"][p_idx];
          std::string node_type = node_obj.value("node_type", "");
          const auto* def = catalog.FindNode(node_type);
          if (def && !def->model_config_field.empty() &&
              node_obj.contains("config") &&
              node_obj["config"].contains(def->model_config_field)) {
            std::string model_id =
                node_obj["config"][def->model_config_field].get<std::string>();
            std::string req_cap = def->model_capability;

            ValidationRemediation rem;
            rem.schema_version = 1;
            rem.cause = (diag->code == DiagnosticCode::kUnknownModelReference)
                            ? "unknown_model_reference"
                            : "model_capability_mismatch";
            rem.facts["model_id"] = model_id;
            rem.facts["required_capability"] = req_cap;

            std::vector<std::string> candidate_model_ids;
            if (root.contains("models") && root["models"].is_array()) {
              for (size_t m_idx = 0; m_idx < root["models"].size(); ++m_idx) {
                const auto& m = root["models"][m_idx];
                if (!m.is_object()) continue;
                std::string mid = m.value("model_id", "");
                if (mid.empty()) continue;
                if (m.value("capability", "") == req_cap) {
                  candidate_model_ids.push_back(mid);
                }
              }
            }
            std::sort(candidate_model_ids.begin(), candidate_model_ids.end());
            rem.facts["candidate_model_ids"] = candidate_model_ids;
            rem.summary = "节点 '" + diag->node_id + "' 引用的模型 '" +
                          model_id + "' 与所需能力 '" + req_cap +
                          "' 不符或未声明。";
            diag->remediation = std::move(rem);
          }
        }
      }
    }
  } else if (diag->code == DiagnosticCode::kMissingInputProducer) {
    size_t consumer_idx = static_cast<size_t>(-1);
    if (diag->path.rfind("/pipeline/", 0) == 0 && root.contains("pipeline") &&
        root["pipeline"].is_array()) {
      size_t idx_end = diag->path.find('/', 10);
      std::string idx_str = (idx_end == std::string::npos)
                                ? diag->path.substr(10)
                                : diag->path.substr(10, idx_end - 10);
      try {
        consumer_idx = std::stoul(idx_str);
      } catch (...) {
        consumer_idx = static_cast<size_t>(-1);
      }
    }

    if (consumer_idx < root.value("pipeline", nlohmann::json::array()).size()) {
      const auto& consumer_node = root["pipeline"][consumer_idx];
      std::string consumer_id = consumer_node.value("id", diag->node_id);
      std::string consumer_type = consumer_node.value("node_type", "");
      const auto* consumer_def = catalog.FindNode(consumer_type);
      std::string port_name = diag->port;
      std::string bound_key = port_name;
      if (consumer_node.contains("ports") &&
          consumer_node["ports"].contains("inputs") &&
          consumer_node["ports"]["inputs"].contains(port_name)) {
        bound_key =
            consumer_node["ports"]["inputs"][port_name].get<std::string>();
      }

      std::string expected_type;
      PortContract expected_contract;
      if (consumer_def) {
        for (const auto& inp : consumer_def->inputs) {
          if (inp.logical_name == port_name) {
            expected_type = inp.type_id;
            expected_contract = inp;
            break;
          }
        }
      }

      int producer_idx = -1;
      std::string producer_id;
      std::string producer_out_type;
      PortContract producer_contract;

      for (size_t p = 0; p < root["pipeline"].size(); ++p) {
        if (p == consumer_idx) continue;
        const auto& p_node = root["pipeline"][p];
        std::string p_id = p_node.value("id", "");
        std::string p_type = p_node.value("node_type", "");
        const auto* p_def = catalog.FindNode(p_type);
        if (!p_def) continue;
        for (const auto& out : p_def->outputs) {
          std::string actual_out_key = out.logical_name;
          if (p_node.contains("ports") && p_node["ports"].contains("outputs") &&
              p_node["ports"]["outputs"].contains(out.logical_name)) {
            actual_out_key =
                p_node["ports"]["outputs"][out.logical_name].get<std::string>();
          }
          if (actual_out_key == bound_key) {
            producer_idx = static_cast<int>(p);
            producer_id = p_id;
            producer_out_type = out.type_id;
            producer_contract = out;
            break;
          }
        }
        if (producer_idx >= 0) break;
      }

      if (producer_idx >= 0) {
        if (producer_out_type == expected_type) {
          ValidationRemediation rem;
          rem.schema_version = 1;
          rem.cause = "producer_not_dependency_ancestor";
          rem.summary = producer_id + " 已输出 " + bound_key +
                        "，但不在消费者的依赖路径中。";
          rem.facts["bound_key"] = bound_key;
          rem.facts["producer_id"] = producer_id;
          diag->remediation = std::move(rem);
        } else {
          ValidationRemediation rem;
          rem.schema_version = 1;
          rem.cause = "port_type_mismatch";
          rem.summary = "生产者 '" + producer_id + "' 输出类型与端口 '" +
                        port_name + "' 要求不符。";
          rem.facts["bound_key"] = bound_key;
          rem.facts["producer_id"] = producer_id;
          rem.facts["expected"] = {
              {"type_id", expected_contract.type_id},
              {"cardinality", expected_contract.cardinality},
              {"provenance_policy", expected_contract.provenance_policy},
              {"lifetime", expected_contract.lifetime}};
          rem.facts["actual"] = {
              {"type_id", producer_contract.type_id},
              {"cardinality", producer_contract.cardinality},
              {"provenance_policy", producer_contract.provenance_policy},
              {"lifetime", producer_contract.lifetime}};
          diag->remediation = std::move(rem);
        }
      } else {
        ValidationRemediation rem;
        rem.schema_version = 1;
        rem.cause = "no_compatible_input_source";
        rem.summary = "Pipeline 中没有为端口 '" + port_name + "' (绑定键: '" +
                      bound_key + "') 提供匹配类型的生产者。";
        rem.facts["bound_key"] = bound_key;
        rem.facts["expected_type"] = expected_type;
        rem.facts["candidate_node_types"] = diag->suggestions;
        diag->remediation = std::move(rem);
      }
    }
  } else if (diag->code == DiagnosticCode::kDuplicateDependency) {
    size_t consumer_idx = static_cast<size_t>(-1);
    if (diag->path.rfind("/pipeline/", 0) == 0 && root.contains("pipeline") &&
        root["pipeline"].is_array()) {
      size_t idx_end = diag->path.find('/', 10);
      std::string idx_str = (idx_end == std::string::npos)
                                ? diag->path.substr(10)
                                : diag->path.substr(10, idx_end - 10);
      try {
        consumer_idx = std::stoul(idx_str);
      } catch (...) {
        consumer_idx = static_cast<size_t>(-1);
      }
    }
    if (consumer_idx < root.value("pipeline", nlohmann::json::array()).size()) {
      const auto& c_node = root["pipeline"][consumer_idx];
      std::string prefix =
          "/pipeline/" + std::to_string(consumer_idx) + "/depends_on/";
      std::string dup_dep;
      if (diag->path.rfind(prefix, 0) == 0 && c_node.contains("depends_on") &&
          c_node["depends_on"].is_array()) {
        try {
          size_t dep_arr_idx = std::stoul(diag->path.substr(prefix.size()));
          if (dep_arr_idx < c_node["depends_on"].size()) {
            dup_dep = c_node["depends_on"][dep_arr_idx].get<std::string>();
          }
        } catch (...) {
        }
      }
      if (!dup_dep.empty()) {
        ValidationRemediation rem;
        rem.schema_version = 1;
        rem.cause = "duplicate_dependency";
        rem.summary =
            "节点 '" + diag->node_id + "' 包含重复依赖 '" + dup_dep + "'。";
        rem.facts["dependency_id"] = dup_dep;
        diag->remediation = std::move(rem);
      }
    }
  } else if (diag->code == DiagnosticCode::kInvalidDependency) {
    size_t consumer_idx = static_cast<size_t>(-1);
    if (diag->path.rfind("/pipeline/", 0) == 0 && root.contains("pipeline") &&
        root["pipeline"].is_array()) {
      size_t idx_end = diag->path.find('/', 10);
      std::string idx_str = (idx_end == std::string::npos)
                                ? diag->path.substr(10)
                                : diag->path.substr(10, idx_end - 10);
      try {
        consumer_idx = std::stoul(idx_str);
      } catch (...) {
        consumer_idx = static_cast<size_t>(-1);
      }
    }
    if (consumer_idx < root.value("pipeline", nlohmann::json::array()).size()) {
      const auto& c_node = root["pipeline"][consumer_idx];
      std::string prefix =
          "/pipeline/" + std::to_string(consumer_idx) + "/depends_on/";
      std::string dep_id;
      if (diag->path.rfind(prefix, 0) == 0 && c_node.contains("depends_on") &&
          c_node["depends_on"].is_array()) {
        try {
          size_t dep_arr_idx = std::stoul(diag->path.substr(prefix.size()));
          if (dep_arr_idx < c_node["depends_on"].size()) {
            dep_id = c_node["depends_on"][dep_arr_idx].get<std::string>();
          }
        } catch (...) {
        }
      }

      std::vector<std::pair<size_t, std::string>> ranked;
      for (const auto& item : root["pipeline"]) {
        std::string valid_id = item.value("id", "");
        if (!valid_id.empty() && valid_id != diag->node_id) {
          ranked.emplace_back(LevenshteinDistance(dep_id, valid_id), valid_id);
        }
      }
      std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
      });
      std::vector<std::string> candidate_node_ids;
      for (const auto& r : ranked) {
        candidate_node_ids.push_back(r.second);
      }

      ValidationRemediation rem;
      rem.schema_version = 1;
      rem.cause = "unknown_dependency";
      rem.summary =
          "节点 '" + diag->node_id + "' 依赖了未知的节点 ID '" + dep_id + "'。";
      rem.facts["dependency_id"] = dep_id;
      rem.facts["candidate_node_ids"] = candidate_node_ids;
      diag->remediation = std::move(rem);
    }
  } else if (diag->code == DiagnosticCode::kMissingBizOutput) {
    ValidationRemediation rem;
    rem.schema_version = 1;
    rem.cause = "missing_biz_output";
    rem.summary = "Pipeline 未产出 biz '" + root.value("biz_name", "") +
                  "' 所需的输出 '" + diag->port + "'。";
    rem.facts["biz_name"] = root.value("biz_name", "");
    rem.facts["bound_key"] = diag->port;
    const auto* biz = catalog.FindBiz(root.value("biz_name", ""));
    if (biz) {
      for (const auto& eg : biz->egress) {
        if (eg.blackboard_key == diag->port) {
          rem.facts["expected_type"] = eg.type_id;
          break;
        }
      }
    }
    diag->remediation = std::move(rem);
  } else if (diag->code == DiagnosticCode::kPortCardinalityMismatch ||
             diag->code == DiagnosticCode::kPortProvenanceMismatch ||
             diag->code == DiagnosticCode::kPortLifetimeMismatch) {
    size_t consumer_idx = static_cast<size_t>(-1);
    if (diag->path.rfind("/pipeline/", 0) == 0 && root.contains("pipeline") &&
        root["pipeline"].is_array()) {
      size_t idx_end = diag->path.find('/', 10);
      std::string idx_str = (idx_end == std::string::npos)
                                ? diag->path.substr(10)
                                : diag->path.substr(10, idx_end - 10);
      try {
        consumer_idx = std::stoul(idx_str);
      } catch (...) {
        consumer_idx = static_cast<size_t>(-1);
      }
    }
    if (consumer_idx < root.value("pipeline", nlohmann::json::array()).size()) {
      const auto& consumer_node = root["pipeline"][consumer_idx];
      std::string consumer_type = consumer_node.value("node_type", "");
      const auto* consumer_def = catalog.FindNode(consumer_type);
      std::string port_name = diag->port;
      std::string bound_key = port_name;
      if (consumer_node.contains("ports") &&
          consumer_node["ports"].contains("inputs") &&
          consumer_node["ports"]["inputs"].contains(port_name)) {
        bound_key =
            consumer_node["ports"]["inputs"][port_name].get<std::string>();
      }

      PortContract expected_contract;
      if (consumer_def) {
        for (const auto& inp : consumer_def->inputs) {
          if (inp.logical_name == port_name) {
            expected_contract = inp;
            break;
          }
        }
      }

      std::string producer_id =
          diag->related_nodes.empty() ? "" : diag->related_nodes[0];
      PortContract producer_contract;

      if (producer_id == "$ingress") {
        const auto* biz = catalog.FindBiz(root.value("biz_name", ""));
        if (biz) {
          for (const auto& ing : biz->ingress) {
            if (ing.blackboard_key == bound_key) {
              producer_contract = ing;
              break;
            }
          }
        }
      } else {
        for (const auto& p_node : root["pipeline"]) {
          if (p_node.value("id", "") == producer_id) {
            std::string p_type = p_node.value("node_type", "");
            const auto* p_def = catalog.FindNode(p_type);
            if (p_def) {
              for (const auto& out : p_def->outputs) {
                std::string actual_out_key = out.logical_name;
                if (p_node.contains("ports") &&
                    p_node["ports"].contains("outputs") &&
                    p_node["ports"]["outputs"].contains(out.logical_name)) {
                  actual_out_key = p_node["ports"]["outputs"][out.logical_name]
                                       .get<std::string>();
                }
                if (actual_out_key == bound_key) {
                  producer_contract = out;
                  break;
                }
              }
            }
            break;
          }
        }
      }

      ValidationRemediation rem;
      rem.schema_version = 1;
      rem.cause = "port_flow_mismatch";
      rem.summary = "生产者 '" + producer_id + "' 与消费者 '" + diag->node_id +
                    "' 在端口 '" + port_name + "' 上的流契约不兼容。";
      rem.facts["bound_key"] = bound_key;
      rem.facts["producer_id"] = producer_id;
      rem.facts["expected"] = {
          {"type_id", expected_contract.type_id},
          {"cardinality", expected_contract.cardinality},
          {"provenance_policy", expected_contract.provenance_policy},
          {"lifetime", expected_contract.lifetime}};
      rem.facts["actual"] = {
          {"type_id", producer_contract.type_id},
          {"cardinality", producer_contract.cardinality},
          {"provenance_policy", producer_contract.provenance_policy},
          {"lifetime", producer_contract.lifetime}};
      diag->remediation = std::move(rem);
    }
  }
}

ValidatedPipelinePlan ValidateAndPlanInternal(
    const nlohmann::json& root, ValidationPolicy policy,
    const PipelineCatalogSnapshot& catalog) {
  ValidatedPipelinePlan plan;
  ValidationReport& report = plan.report;

  auto finish_plan = [&](ValidatedPipelinePlan& p) -> ValidatedPipelinePlan& {
    for (auto& diag : p.report.diagnostics) {
      PopulateBasicRemediation(&diag, root, catalog, p.report.diagnostics);
    }
    p.report.ok = p.report.diagnostics.empty();
    return p;
  };

  PipelineDiagnostic parse_diag;
  if (!ParsePipelineConfig(root, &plan.config, &parse_diag)) {
    Add(&report, PipelineErrorCodeToDiagnosticCode(parse_diag.code),
        parse_diag.path, parse_diag.message);
    return finish_plan(plan);
  }
  const auto& parsed = plan.config;
  const auto* biz = catalog.FindBiz(parsed.biz_name);
  if (!biz && policy == ValidationPolicy::kStrict) {
    Add(&report, DiagnosticCode::kUnknownBiz, "/biz_name",
        "No registered biz contract accepts pipeline name: " + parsed.biz_name);
  }
  if (NodeRegistry::Instance().HasConflict()) {
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
      const bool backend_fields_valid = ValidateAndNormalizeConfig(
          backend_def_opt->config_fields, model.backend_config,
          &normalized_bcfg, &bcfg_diags,
          "/models/" + std::to_string(model.source_index) + "/backend_config",
          DiagnosticCode::kUnknownBackendConfigField);
      for (auto& d : bcfg_diags) {
        report.diagnostics.push_back(std::move(d));
      }

      if (backend_fields_valid && backend_def_opt->validate_config) {
        const std::string path =
            "/models/" + std::to_string(model.source_index) + "/backend_config";
        std::string diagnostic;
        try {
          if (!backend_def_opt->validate_config(normalized_bcfg, &diagnostic)) {
            Add(&report, DiagnosticCode::kInvalidCombination, path,
                diagnostic.empty() ? "Invalid backend configuration"
                                   : diagnostic);
          }
        } catch (const std::exception& e) {
          Add(&report, DiagnosticCode::kInvalidCombination, path, e.what());
        } catch (...) {
          Add(&report, DiagnosticCode::kInvalidCombination, path,
              "Backend configuration validator threw an unknown exception");
        }
      }

      // 8. Orchestration only performs environment-neutral lexical path checks.
      // Deployment roots are resolved by Integration before runtime validation.
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
    const auto* definition = catalog.FindNode(node.node_type);
    bool factory_has = NodeRegistry::Instance().Has(node.node_type);
    if (!factory_has || (!definition && policy == ValidationPolicy::kStrict)) {
      Add(&report, DiagnosticCode::kUnknownNodeType,
          "/pipeline/" + std::to_string(node.source_index) + "/node_type",
          "Unknown node_type or missing catalog definition: " + node.node_type,
          node.id);
      continue;
    }
    if (definition) {
      def_by_id[node.id] = definition;

      if (biz && !definition->biz_names.empty() &&
          std::find(definition->biz_names.begin(), definition->biz_names.end(),
                    parsed.biz_name) == definition->biz_names.end()) {
        Add(&report, DiagnosticCode::kNodeBizMismatch,
            "/pipeline/" + std::to_string(node.source_index) + "/node_type",
            "Node type is not declared for biz: " + parsed.biz_name, node.id);
      }

      bool node_fields_valid = false;
      auto normalized_config = ValidateConfigFields(
          definition->config_fields, node.config,
          "/pipeline/" + std::to_string(node.source_index) + "/config", node.id,
          &report, &node_fields_valid);
      if (node_fields_valid && definition->validate_config) {
        std::unordered_set<std::string> connected;
        for (const auto& binding : node.ports.inputs)
          connected.insert(binding.first);
        std::string diagnostic;
        try {
          if (!definition->validate_config(normalized_config, connected,
                                           &diagnostic)) {
            Add(&report, DiagnosticCode::kInvalidCombination,
                "/pipeline/" + std::to_string(node.source_index) + "/config",
                diagnostic.empty() ? "Invalid node configuration" : diagnostic,
                node.id);
          }
        } catch (const std::exception& e) {
          Add(&report, DiagnosticCode::kInvalidCombination,
              "/pipeline/" + std::to_string(node.source_index) + "/config",
              e.what(), node.id);
        } catch (...) {
          Add(&report, DiagnosticCode::kInvalidCombination,
              "/pipeline/" + std::to_string(node.source_index) + "/config",
              "Node configuration validator threw an unknown exception",
              node.id);
        }
      }
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
      (policy == ValidationPolicy::kStrict && !biz) ||
      report.topological_order.size() != nodes.size()) {
    return finish_plan(plan);
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

  std::unordered_map<std::string, BizPortDefinition> ingress;
  if (biz) {
    for (const auto& port : biz->ingress) ingress[port.blackboard_key] = port;
  }
  std::unordered_map<std::string,
                     std::vector<std::pair<std::string, PortContract>>>
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
      bool declared = std::any_of(
          definition.inputs.begin(), definition.inputs.end(),
          [&](const auto& item) { return item.logical_name == in_port; });
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
      bool declared = std::any_of(
          definition.outputs.begin(), definition.outputs.end(),
          [&](const auto& item) { return item.logical_name == out_port; });
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
      std::string actual_key = input.logical_name;
      bool explicitly_bound = false;
      auto port_it = node.ports.inputs.find(input.logical_name);
      if (port_it != node.ports.inputs.end()) {
        actual_key = port_it->second;
        explicitly_bound = true;
        bound_input_ports.insert(input.logical_name);
      } else if (input.required) {
        bound_input_ports.insert(input.logical_name);
      }

      if (!input.required && !explicitly_bound) continue;
      node_plan.ports.push_back({input.logical_name, actual_key, input.type_id,
                                 input.cardinality, input.provenance_policy,
                                 input.lifetime, PortDirection::kInput});

      const std::string input_path = "/pipeline/" +
                                     std::to_string(node.source_index) +
                                     "/ports/inputs/" + input.logical_name;
      bool found = false;
      auto producer_it = producers.find(actual_key);
      if (producer_it != producers.end()) {
        // Blackboard keys are write-once. Duplicate producers are diagnosed
        // below; resolve the nearest topologically preceding ancestor for
        // deterministic type and flow-contract diagnostics.
        for (auto it = producer_it->second.rbegin();
             it != producer_it->second.rend(); ++it) {
          if (is_ancestor(it->first, id)) {
            if (it->second.type_id == input.type_id) {
              found = true;
              ValidatePortFlowContract(it->second, input, input_path, id,
                                       input.logical_name, it->first, &report);
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
          ValidatePortFlowContract(root_port->second, input, input_path, id,
                                   input.logical_name, "$ingress", &report);
        }
      }
      if (!found && biz) {
        std::vector<std::string> suggestions;
        for (const auto& candidate : catalog.nodes) {
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
                   "/ports/inputs/" + input.logical_name)
                : ("/pipeline/" + std::to_string(node.source_index)),
            "No biz ingress or ancestor node produces port '" +
                input.logical_name + "' (bound key: '" + actual_key +
                "') of type '" + input.type_id + "'",
            id, input.logical_name, {}, suggestions);
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
      std::string actual_key = output.logical_name;
      auto port_it = node.ports.outputs.find(output.logical_name);
      if (port_it != node.ports.outputs.end()) {
        actual_key = port_it->second;
      }

      const std::string output_path =
          "/pipeline/" + std::to_string(node.source_index) +
          (port_it == node.ports.outputs.end()
               ? std::string()
               : "/ports/outputs/" + output.logical_name);

      node_plan.ports.push_back(
          {output.logical_name, actual_key, output.type_id, output.cardinality,
           output.provenance_policy, output.lifetime, PortDirection::kOutput});

      auto& existing = producers[actual_key];
      std::vector<std::string> conflicting_producers;
      if (ingress.find(actual_key) != ingress.end()) {
        conflicting_producers.push_back("$ingress");
      }
      if (!existing.empty()) {
        conflicting_producers.push_back(existing.back().first);
      }
      if (!conflicting_producers.empty()) {
        Add(&report, DiagnosticCode::kDuplicatePortProducer, output_path,
            "Write-once Blackboard port has multiple producers: " + actual_key,
            id, output.logical_name, std::move(conflicting_producers));
      }
      existing.push_back({id, static_cast<const PortContract&>(output)});
    }

    plan.node_plans[id] = std::move(node_plan);
  }

  if (biz) {
    for (const auto& consumer : biz->egress) {
      auto it = producers.find(consumer.blackboard_key);
      if (it == producers.end() || it->second.empty()) {
        if (consumer.required) {
          Add(&report, DiagnosticCode::kMissingBizOutput, "/pipeline",
              "Pipeline does not produce required biz output: " +
                  consumer.blackboard_key,
              {}, consumer.blackboard_key);
        }
        continue;
      }
      const auto& [producer_id, producer_port] = it->second.back();
      const auto& producer_node = *node_by_id.at(producer_id);
      std::string output_path =
          "/pipeline/" + std::to_string(producer_node.source_index);
      for (const auto& [logical_key, actual_key] :
           producer_node.ports.outputs) {
        if (actual_key == consumer.blackboard_key) {
          output_path += "/ports/outputs/" + logical_key;
          break;
        }
      }
      if (producer_port.type_id != consumer.type_id) {
        Add(&report, DiagnosticCode::kMissingBizOutput, output_path,
            "Biz output type mismatch for '" + consumer.blackboard_key +
                "': expected '" + consumer.type_id + "', got '" +
                producer_port.type_id + "'",
            producer_id, consumer.blackboard_key, {"$egress"});
        continue;
      }
      ValidatePortFlowContract(producer_port, consumer, output_path,
                               producer_id, consumer.blackboard_key, "$egress",
                               &report);
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

  return finish_plan(plan);
}

}  // namespace

nlohmann::json ValidationFix::ToJson() const {
  return {{"id", id},
          {"title", title},
          {"effect", effect},
          {"patch", patch},
          {"verification", verification}};
}

nlohmann::json ValidationRemediation::ToJson() const {
  nlohmann::json fixes_json = nlohmann::json::array();
  for (const auto& fix : fixes) {
    fixes_json.push_back(fix.ToJson());
  }
  return {{"schema_version", schema_version},
          {"cause", cause},
          {"summary", summary},
          {"facts", facts},
          {"fixes", std::move(fixes_json)}};
}

nlohmann::json ValidationDiagnostic::ToJson() const {
  nlohmann::json item = {{"code", DiagnosticCodeName(code)},
                         {"path", path},
                         {"message", message},
                         {"severity", severity}};
  if (!node_id.empty()) item["node_id"] = node_id;
  if (!port.empty()) item["port"] = port;
  if (!related_nodes.empty()) item["related_nodes"] = related_nodes;
  if (!suggestions.empty()) item["suggestions"] = suggestions;
  if (remediation.has_value()) {
    item["remediation"] = remediation->ToJson();
  }
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
  const auto catalog = PipelineCatalog::Snapshot();
  return ValidateAndPlanInternal(root, policy, catalog);
}

ValidationReport PipelineValidator::Validate(const nlohmann::json& root,
                                             ValidationPolicy policy) {
  return ValidateAndPlan(root, policy).report;
}

ValidationReport PipelineValidator::Explain(const nlohmann::json& root,
                                            ValidationPolicy policy) {
  const auto catalog = PipelineCatalog::Snapshot();
  ValidationReport report =
      ValidateAndPlanInternal(root, policy, catalog).report;
  if (report.ok) {
    return report;
  }

  if (!root.is_object() || !root.contains("pipeline") ||
      !root["pipeline"].is_array()) {
    return report;
  }

  constexpr size_t kMaxVerificationAttempts = 8;
  constexpr size_t kMaxFixesPerDiagnostic = 3;
  constexpr size_t kMaxFixesPerReport = 8;
  size_t total_verification_attempts = 0;
  size_t total_fixes_accepted = 0;
  size_t fix_counter = 0;

  std::vector<DiagnosticIdentity> orig_identities;
  orig_identities.reserve(report.diagnostics.size());
  for (const auto& d : report.diagnostics) {
    orig_identities.push_back(GetDiagnosticIdentity(d));
  }

  for (auto& diag : report.diagnostics) {
    if (total_verification_attempts >= kMaxVerificationAttempts ||
        total_fixes_accepted >= kMaxFixesPerReport) {
      break;
    }
    if (!diag.remediation.has_value()) continue;

    std::vector<ValidationFix> candidate_fixes;

    if (diag.remediation->cause == "unknown_config_field") {
      size_t idx_end = diag.path.find('/', 10);
      size_t p_idx = std::stoul(diag.path.substr(10, idx_end - 10));
      const auto& node_obj = root["pipeline"][p_idx];
      std::string node_type = node_obj.value("node_type", "");
      const auto* def = catalog.FindNode(node_type);
      std::string prefix = "/pipeline/" + std::to_string(p_idx) + "/config/";
      std::string field_name = diag.remediation->facts.value("field", "");
      if (def && node_obj.contains("config") &&
          node_obj["config"].is_object() &&
          node_obj["config"].contains(field_name)) {
        const auto& original_val = node_obj["config"][field_name];
        auto candidate_fields = diag.remediation->facts.value(
            "candidate_fields", std::vector<std::string>{});
        for (const auto& cand : candidate_fields) {
          if (node_obj["config"].contains(cand)) continue;
          auto cf_it =
              std::find_if(def->config_fields.begin(), def->config_fields.end(),
                           [&](const auto& f) { return f.name == cand; });
          if (cf_it != def->config_fields.end() &&
              ValueMatchesConfigKind(original_val, cf_it->kind)) {
            ValidationFix fix;
            fix.id = "rename-config-field-" + std::to_string(++fix_counter);
            fix.title = "将 '" + field_name + "' 重命名为 '" + cand + "'";
            fix.effect = "更正字段名称为已知配置项 '" + cand + "'。";
            std::string from_path = prefix + EscapeJsonPointer(field_name);
            std::string to_path = prefix + EscapeJsonPointer(cand);
            fix.patch = nlohmann::json::array(
                {{{"op", "test"}, {"path", from_path}, {"value", original_val}},
                 {{"op", "move"}, {"from", from_path}, {"path", to_path}}});
            candidate_fixes.push_back(std::move(fix));
          }
        }
      }
    } else if (diag.remediation->cause == "unknown_model_reference" ||
               diag.remediation->cause == "model_capability_mismatch") {
      size_t idx_end = diag.path.find('/', 10);
      size_t p_idx = std::stoul(diag.path.substr(10, idx_end - 10));
      const auto& node_obj = root["pipeline"][p_idx];
      std::string node_type = node_obj.value("node_type", "");
      const auto* def = catalog.FindNode(node_type);
      std::string model_id = diag.remediation->facts.value("model_id", "");
      auto candidate_model_ids = diag.remediation->facts.value(
          "candidate_model_ids", std::vector<std::string>{});
      if (def && !def->model_config_field.empty()) {
        std::string model_path = "/pipeline/" + std::to_string(p_idx) +
                                 "/config/" +
                                 EscapeJsonPointer(def->model_config_field);
        for (const auto& cand_id : candidate_model_ids) {
          if (cand_id == model_id) continue;
          ValidationFix fix;
          fix.id = "use-model-" + std::to_string(++fix_counter);
          fix.title = "使用模型 '" + cand_id + "'";
          fix.effect =
              "将模型引用从 '" + model_id + "' 更改为 '" + cand_id + "'。";
          fix.patch = nlohmann::json::array(
              {{{"op", "test"}, {"path", model_path}, {"value", model_id}},
               {{"op", "replace"}, {"path", model_path}, {"value", cand_id}}});
          candidate_fixes.push_back(std::move(fix));
        }
      }
    } else if (diag.remediation->cause == "producer_not_dependency_ancestor") {
      size_t idx_end = diag.path.find('/', 10);
      size_t consumer_idx = std::stoul(diag.path.substr(10, idx_end - 10));
      const auto& consumer_node = root["pipeline"][consumer_idx];
      std::string consumer_id = consumer_node.value("id", diag.node_id);
      std::string producer_id =
          diag.remediation->facts.value("producer_id", "");
      int producer_idx = -1;
      for (size_t p = 0; p < root["pipeline"].size(); ++p) {
        if (root["pipeline"][p].value("id", "") == producer_id) {
          producer_idx = static_cast<int>(p);
          break;
        }
      }
      if (producer_idx >= 0) {
        ValidationFix fix;
        fix.id = "add-dependency-" + std::to_string(++fix_counter);
        fix.title = "添加对 " + producer_id + " 的依赖";
        fix.effect = "消费者等待 " + producer_id + " 完成后读取其结果。";

        std::string p_path = "/pipeline/" + std::to_string(producer_idx);
        std::string c_path = "/pipeline/" + std::to_string(consumer_idx);

        nlohmann::json patch = nlohmann::json::array();
        patch.push_back(
            {{"op", "test"}, {"path", p_path + "/id"}, {"value", producer_id}});
        patch.push_back(
            {{"op", "test"}, {"path", c_path + "/id"}, {"value", consumer_id}});

        if (consumer_node.contains("depends_on") &&
            consumer_node["depends_on"].is_array()) {
          patch.push_back({{"op", "test"},
                           {"path", c_path + "/depends_on"},
                           {"value", consumer_node["depends_on"]}});
          patch.push_back({{"op", "add"},
                           {"path", c_path + "/depends_on/-"},
                           {"value", producer_id}});
        } else {
          patch.push_back({{"op", "add"},
                           {"path", c_path + "/depends_on"},
                           {"value", nlohmann::json::array({producer_id})}});
        }
        fix.patch = std::move(patch);
        candidate_fixes.push_back(std::move(fix));
      }
    } else if (diag.remediation->cause == "duplicate_dependency") {
      size_t idx_end = diag.path.find('/', 10);
      size_t consumer_idx = std::stoul(diag.path.substr(10, idx_end - 10));
      const auto& c_node = root["pipeline"][consumer_idx];
      std::string dup_dep = diag.remediation->facts.value("dependency_id", "");
      std::string prefix =
          "/pipeline/" + std::to_string(consumer_idx) + "/depends_on/";
      if (diag.path.rfind(prefix, 0) == 0 && c_node.contains("depends_on") &&
          c_node["depends_on"].is_array() && !dup_dep.empty()) {
        ValidationFix fix;
        fix.id = "remove-duplicate-dep-" + std::to_string(++fix_counter);
        fix.title = "删除重复依赖 '" + dup_dep + "'";
        fix.effect = "移除对 '" + dup_dep + "' 的重复依赖声明。";
        fix.patch = nlohmann::json::array(
            {{{"op", "test"}, {"path", diag.path}, {"value", dup_dep}},
             {{"op", "remove"}, {"path", diag.path}}});
        candidate_fixes.push_back(std::move(fix));
      }
    } else if (diag.remediation->cause == "unknown_dependency") {
      size_t idx_end = diag.path.find('/', 10);
      size_t consumer_idx = std::stoul(diag.path.substr(10, idx_end - 10));
      const auto& c_node = root["pipeline"][consumer_idx];
      std::string prefix =
          "/pipeline/" + std::to_string(consumer_idx) + "/depends_on/";
      std::string dep_id = diag.remediation->facts.value("dependency_id", "");
      auto candidate_node_ids = diag.remediation->facts.value(
          "candidate_node_ids", std::vector<std::string>{});
      if (diag.path.rfind(prefix, 0) == 0 && c_node.contains("depends_on") &&
          c_node["depends_on"].is_array() && !candidate_node_ids.empty()) {
        for (const auto& cand_id : candidate_node_ids) {
          ValidationFix fix;
          fix.id = "replace-dependency-" + std::to_string(++fix_counter);
          fix.title = "将依赖 '" + dep_id + "' 更改为 '" + cand_id + "'";
          fix.effect = "更正依赖为已知节点 '" + cand_id + "'。";
          fix.patch = nlohmann::json::array(
              {{{"op", "test"}, {"path", diag.path}, {"value", dep_id}},
               {{"op", "replace"}, {"path", diag.path}, {"value", cand_id}}});
          candidate_fixes.push_back(std::move(fix));
        }
      }
    }

    size_t verified_for_diag = 0;
    for (auto& fix : candidate_fixes) {
      if (total_verification_attempts >= kMaxVerificationAttempts ||
          total_fixes_accepted >= kMaxFixesPerReport) {
        break;
      }
      if (verified_for_diag >= kMaxFixesPerDiagnostic) break;

      nlohmann::json patched_root = root;
      bool patch_ok = false;
      try {
        patched_root = patched_root.patch(fix.patch);
        patch_ok = true;
      } catch (...) {
        patch_ok = false;
      }
      if (!patch_ok) continue;

      total_verification_attempts++;
      ValidationReport new_report =
          ValidateAndPlanInternal(patched_root, policy, catalog).report;
      if (new_report.ok) {
        fix.verification = "pipeline_valid";
        diag.remediation->fixes.push_back(std::move(fix));
        total_fixes_accepted++;
        verified_for_diag++;
      } else {
        bool target_still_present = false;
        const auto target_id = GetDiagnosticIdentity(diag);
        for (const auto& nd : new_report.diagnostics) {
          if (GetDiagnosticIdentity(nd) == target_id) {
            target_still_present = true;
            break;
          }
        }
        if (!target_still_present) {
          bool has_new_error = false;
          for (const auto& nd : new_report.diagnostics) {
            const auto nd_id = GetDiagnosticIdentity(nd);
            if (std::none_of(
                    orig_identities.begin(), orig_identities.end(),
                    [&](const auto& od_id) { return od_id == nd_id; })) {
              has_new_error = true;
              break;
            }
          }
          if (!has_new_error) {
            bool candidate_model_has_error = false;
            if (diag.remediation->cause == "unknown_model_reference" ||
                diag.remediation->cause == "model_capability_mismatch") {
              std::string cand_mid;
              for (const auto& op : fix.patch) {
                if (op.value("op", "") == "replace") {
                  cand_mid = op.value("value", "");
                  break;
                }
              }
              if (patched_root.contains("models") &&
                  patched_root["models"].is_array()) {
                for (size_t m_idx = 0; m_idx < patched_root["models"].size();
                     ++m_idx) {
                  if (patched_root["models"][m_idx].value("model_id", "") ==
                      cand_mid) {
                    std::string m_pfx = "/models/" + std::to_string(m_idx);
                    for (const auto& nd : new_report.diagnostics) {
                      if (nd.path == m_pfx ||
                          nd.path.rfind(m_pfx + "/", 0) == 0) {
                        candidate_model_has_error = true;
                        break;
                      }
                    }
                    break;
                  }
                }
              }
            }
            if (!candidate_model_has_error) {
              fix.verification = "target_resolved";
              diag.remediation->fixes.push_back(std::move(fix));
              total_fixes_accepted++;
              verified_for_diag++;
            }
          }
        }
      }
    }
  }

  return report;
}

}  // namespace llm_edgeflow
