#include "pipeline_remediation.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "contracts/json_pointer.h"
#include "core/name_suggestions.h"
#include "core/pipeline_catalog.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {

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
    case ConfigValueKind::kMap:
    case ConfigValueKind::kObject:
      return value.is_object();
    case ConfigValueKind::kArray:
      return value.is_array();
    case ConfigValueKind::kJson:
      return !value.is_null();
  }
  return false;
}

struct DiagnosticIdentity {
  DiagnosticCode code;
  std::string node_name;
  std::string port;
  std::string subpath;

  bool operator==(const DiagnosticIdentity& other) const {
    return code == other.code && node_name == other.node_name &&
           port == other.port && subpath == other.subpath;
  }
};

DiagnosticIdentity GetDiagnosticIdentity(const ValidationDiagnostic& d) {
  DiagnosticIdentity id;
  id.code = d.code;
  id.node_name = d.node_name;
  id.port = d.port;
  if (d.code == DiagnosticCode::kDuplicateDependency) {
    if (!d.related_nodes.empty()) {
      id.subpath = "/depends_on/" + d.related_nodes.front();
    } else if (d.remediation.has_value() &&
               d.remediation->facts.contains("dependency_name")) {
      id.subpath = "/depends_on/" +
                   d.remediation->facts["dependency_name"].get<std::string>();
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

std::string DescribeItemShape(const nlohmann::json& shape) {
  const auto kind = shape.value("kind", "unknown");
  if (kind == "per_request") return "每请求一项";
  if (kind == "multi") {
    return "由 '" + shape.value("origin", "") + "' 产生的每请求零到多项";
  }
  return "每请求项数未知";
}

void PopulateBasicRemediation(ValidationDiagnostic* diag,
                              const nlohmann::json& root,
                              const PipelineCatalogSnapshot& catalog,
                              const PipelineIoBoundary* io_boundary = nullptr) {
  if (!diag || diag->remediation.has_value()) return;
  if (!root.is_object()) return;

  if (diag->code == DiagnosticCode::kUnknownNodeType ||
      diag->code == DiagnosticCode::kUnknownModelType ||
      diag->code == DiagnosticCode::kUnknownBackend ||
      diag->code == DiagnosticCode::kBackendProtocolMismatch) {
    const nlohmann::json::json_pointer pointer(diag->path);
    if (!root.contains(pointer) || !root.at(pointer).is_string()) return;
    const auto item_path = diag->path.rfind("/models/", 0) == 0
                               ? diag->path.substr(0, diag->path.find('/', 8))
                               : diag->path.substr(0, diag->path.rfind('/'));
    const auto& item = root.at(nlohmann::json::json_pointer(item_path));
    const std::string name = root.at(pointer).get<std::string>();
    ValidationRemediation rem;
    std::vector<std::string> names;
    std::string candidates_key;
    std::string next_step;
    if (diag->code == DiagnosticCode::kUnknownNodeType) {
      rem.cause = RemediationCause::kUnknownNodeType;
      rem.facts["node_type"] = name;
      candidates_key = "candidate_node_types";
      rem.summary = "节点 '" + diag->node_name + "' 的 type '" + name;
      next_step = "新增的 Node 需要重新构建后才会注册。";
    } else {
      const std::string model_name = item.value("name", "");
      rem.facts["model_name"] = model_name;
      if (diag->code == DiagnosticCode::kUnknownModelType) {
        rem.cause = RemediationCause::kUnknownModelType;
        rem.facts["model_type"] = name;
        candidates_key = "candidate_model_types";
        for (const auto& def : PipelineCatalog::Models())
          names.push_back(def.model_type);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        rem.facts["registered_model_types"] = names;
        rem.summary = "模型 '" + model_name + "' 的 type '" + name;
      } else {
        const bool mismatch =
            diag->code == DiagnosticCode::kBackendProtocolMismatch;
        rem.cause = mismatch ? RemediationCause::kBackendProtocolMismatch
                             : RemediationCause::kUnknownBackend;
        rem.facts["model_type"] = item.value("type", "");
        rem.facts["backend_type"] = name;
        candidates_key = "candidate_backends";
        rem.facts["registered_backends"] = diag->suggestions;
        rem.summary = "模型 '" + model_name + "' 的 backend.type '" + name;
        if (mismatch) {
          rem.summary +=
              "' 没有类别 '" + item.value("type", "") + "' 的兼容实现。";
          rem.facts[candidates_key] = diag->suggestions;
          diag->remediation = std::move(rem);
          return;
        }
        next_step = "可选 Backend 需要在构建时启用。";
      }
    }
    rem.facts[candidates_key] = diag->suggestions;
    rem.summary += "' 未在当前构建中注册。";
    if (!diag->suggestions.empty()) {
      rem.summary += "相近的已注册类型：";
      for (size_t i = 0; i < diag->suggestions.size(); ++i) {
        if (i) rem.summary += "、";
        rem.summary += diag->suggestions[i];
      }
      rem.summary += "。";
    }
    rem.summary += next_step;
    diag->remediation = std::move(rem);
    return;
  }

  const bool config_diagnostic =
      diag->code == DiagnosticCode::kUnknownConfigField ||
      diag->code == DiagnosticCode::kMissingConfigField ||
      diag->code == DiagnosticCode::kConfigFieldType ||
      diag->code == DiagnosticCode::kConfigFieldRange ||
      diag->code == DiagnosticCode::kConfigFieldEnum;
  if (config_diagnostic && diag->path.rfind("/models/", 0) == 0 &&
      root.contains("models") && root["models"].is_array()) {
    const size_t idx_end = diag->path.find('/', 8);
    if (idx_end == std::string::npos) return;
    size_t model_index = 0;
    try {
      model_index = std::stoul(diag->path.substr(8, idx_end - 8));
    } catch (...) {
      return;
    }
    if (model_index >= root["models"].size()) return;
    const auto& model = root["models"][model_index];
    if (!model.is_object()) return;
    const auto backend = model.find("backend");
    if (backend == model.end() || !backend->is_object()) return;
    const std::string model_prefix = "/models/" + std::to_string(model_index);
    std::string params_path = model_prefix + "/params";
    std::vector<ConfigFieldDefinition> fields;
    if (diag->path.rfind(params_path + "/", 0) == 0) {
      const auto implementations = ModelRegistry::Instance().FindImplementation(
          model.value("type", ""), backend->value("type", ""));
      if (implementations.size() != 1) return;
      fields = implementations.front().params.Fields();
    } else {
      params_path = model_prefix + "/backend/params";
      if (diag->path.rfind(params_path + "/", 0) != 0) return;
      const auto definition =
          PipelineCatalog::FindBackend(backend->value("type", ""));
      if (!definition) return;
      fields = definition->params.Fields();
    }
    const std::string field_name =
        nlohmann::json::json_pointer(diag->path).back();
    ValidationRemediation rem;
    rem.facts["model_name"] = model.value("name", "");
    rem.facts["params_path"] = params_path;
    rem.facts["field"] = field_name;
    rem.summary =
        "模型 '" + model.value("name", "") + "' 的参数 '" + field_name + "' ";
    if (diag->code == DiagnosticCode::kUnknownConfigField) {
      rem.cause = RemediationCause::kUnknownConfigField;
      rem.summary += "未声明。";
      std::vector<std::string> names;
      for (const auto& field : fields) names.push_back(field.name);
      rem.facts["candidate_fields"] =
          RankByEditDistance(field_name, std::move(names));
    } else {
      const bool missing = diag->code == DiagnosticCode::kMissingConfigField;
      rem.cause = missing ? RemediationCause::kMissingConfigField
                          : RemediationCause::kInvalidConfigValue;
      rem.summary += missing ? "未提供。" : "值不合法。";
      const auto field = std::find_if(
          fields.begin(), fields.end(),
          [&](const auto& item) { return item.name == field_name; });
      if (field != fields.end()) {
        rem.facts["expected_type"] = ConfigValueKindName(field->kind);
        if (field->minimum) rem.facts["minimum"] = *field->minimum;
        if (field->maximum) rem.facts["maximum"] = *field->maximum;
        if (!field->enum_values.empty()) rem.facts["enum"] = field->enum_values;
      }
    }
    diag->remediation = std::move(rem);
    return;
  }

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
          std::string node_type = node_obj.value("type", "");
          const auto* def = catalog.FindNode(node_type);
          std::string prefix =
              "/pipeline/" + std::to_string(p_idx) + "/params/";
          if (def && diag->path.rfind(prefix, 0) == 0) {
            std::string field_name = diag->path.substr(prefix.size());
            ValidationRemediation rem;
            rem.cause = RemediationCause::kUnknownConfigField;
            rem.summary = "节点 '" + diag->node_name +
                          "' 的配置包含未知字段 '" + field_name + "'。";
            rem.facts["field"] = field_name;

            std::vector<std::string> names;
            for (const auto& field : def->config_fields)
              names.push_back(field.name);
            rem.facts["candidate_fields"] =
                RankByEditDistance(field_name, std::move(names));
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
          std::string node_type = node_obj.value("type", "");
          const auto* def = catalog.FindNode(node_type);
          std::string prefix =
              "/pipeline/" + std::to_string(p_idx) + "/params/";
          std::string field_name;
          if (diag->path.rfind(prefix, 0) == 0) {
            field_name = diag->path.substr(prefix.size());
          }
          if (def && !field_name.empty()) {
            ValidationRemediation rem;
            rem.cause = RemediationCause::kMissingConfigField;
            rem.summary = "节点 '" + diag->node_name + "' 缺少必填配置字段 '" +
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
          std::string node_type = node_obj.value("type", "");
          const auto* def = catalog.FindNode(node_type);
          std::string prefix =
              "/pipeline/" + std::to_string(p_idx) + "/params/";
          std::string field_name;
          if (diag->path.rfind(prefix, 0) == 0) {
            field_name = diag->path.substr(prefix.size());
          }
          if (def && !field_name.empty()) {
            ValidationRemediation rem;
            rem.cause = RemediationCause::kInvalidConfigValue;
            rem.summary = "节点 '" + diag->node_name + "' 的配置项 '" +
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
             diag->code == DiagnosticCode::kModelTypeMismatch) {
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
          std::string node_type = node_obj.value("type", "");
          const auto* def = catalog.FindNode(node_type);
          if (def) {
            std::string expected_prefix =
                "/pipeline/" + std::to_string(p_idx) + "/params/";
            for (const auto& dep : def->model_dependencies) {
              if (diag->path !=
                  expected_prefix + EscapeJsonPointer(dep.config_field)) {
                continue;
              }
              std::string model_name =
                  (node_obj.contains("params") &&
                   node_obj["params"].contains(dep.config_field) &&
                   node_obj["params"][dep.config_field].is_string())
                      ? node_obj["params"][dep.config_field].get<std::string>()
                      : "";
              std::string required_model_type = dep.model_type;

              ValidationRemediation rem;
              rem.cause = (diag->code == DiagnosticCode::kUnknownModelReference)
                              ? RemediationCause::kUnknownModelReference
                              : RemediationCause::kModelTypeMismatch;
              rem.facts["model_name"] = model_name;
              rem.facts["required_model_type"] = required_model_type;

              std::vector<std::string> candidate_model_names;
              if (root.contains("models") && root["models"].is_array()) {
                for (size_t m_idx = 0; m_idx < root["models"].size(); ++m_idx) {
                  const auto& m = root["models"][m_idx];
                  if (!m.is_object()) continue;
                  std::string mid = m.value("name", "");
                  if (mid.empty()) continue;
                  if (m.value("type", "") == required_model_type) {
                    candidate_model_names.push_back(mid);
                  }
                }
              }
              std::sort(candidate_model_names.begin(),
                        candidate_model_names.end());
              rem.facts["candidate_model_names"] = candidate_model_names;
              rem.summary = "节点 '" + diag->node_name + "' 引用的模型 '" +
                            model_name + "' 与所需能力 '" +
                            required_model_type + "' 不符或未声明。";
              diag->remediation = std::move(rem);
              break;
            }
          }
        }
      }
    }
  } else if (diag->code == DiagnosticCode::kMissingInputProducer ||
             diag->code == DiagnosticCode::kMissingOutputProducer ||
             diag->code == DiagnosticCode::kUnknownNodeReference ||
             diag->code == DiagnosticCode::kUnknownPortReference ||
             diag->code == DiagnosticCode::kPortTypeMismatch) {
    std::string bound_key;
    std::string expected_type;
    const nlohmann::json::json_pointer pointer(diag->path);
    if (root.contains(pointer) && root.at(pointer).is_string())
      bound_key = root.at(pointer).get<std::string>();
    if (diag->path.rfind("/pipeline/", 0) == 0 && root.contains("pipeline") &&
        root["pipeline"].is_array()) {
      const auto idx_end = diag->path.find('/', 10);
      try {
        const auto index = std::stoul(diag->path.substr(10, idx_end - 10));
        if (index < root["pipeline"].size()) {
          const auto& node = root["pipeline"][index];
          const auto* definition = catalog.FindNode(node.value("type", ""));
          if (definition) {
            for (const auto& port : definition->inputs)
              if (port.logical_name == diag->port) {
                expected_type = port.type_id;
                break;
              }
          }
        }
      } catch (...) {
        return;
      }
    } else if (io_boundary) {
      for (const auto& port : io_boundary->output_consumed_ports) {
        const auto path = port.path.empty() ? "/io/output" : port.path;
        if (port.logical_name == diag->port &&
            (diag->path == path ||
             diag->path == path + "/" + EscapeJsonPointer(port.logical_name))) {
          bound_key = port.blackboard_key;
          expected_type = port.type_id;
          break;
        }
      }
    }
    if (expected_type.empty() && diag->facts.contains("expected") &&
        diag->facts["expected"].is_object())
      expected_type = diag->facts["expected"].value("type_id", "");

    ValidationRemediation rem;
    rem.facts = diag->facts;
    rem.facts["bound_key"] = bound_key;
    rem.facts["expected_type"] = expected_type;
    rem.summary = diag->message;
    const auto dot = bound_key.find('.');
    const auto producer_name = bound_key.substr(0, dot);
    if (diag->code == DiagnosticCode::kUnknownNodeReference) {
      rem.cause = RemediationCause::kUnknownNodeReference;
      rem.facts["producer_name"] = producer_name;
      rem.facts["candidate_node_names"] = diag->suggestions;
    } else if (diag->code == DiagnosticCode::kUnknownPortReference) {
      rem.cause = RemediationCause::kUnknownPortReference;
      rem.facts["producer_name"] = producer_name;
      rem.facts["candidate_ports"] = diag->suggestions;
    } else {
      rem.cause = diag->code == DiagnosticCode::kPortTypeMismatch
                      ? RemediationCause::kPortTypeMismatch
                  : diag->code == DiagnosticCode::kMissingOutputProducer
                      ? RemediationCause::kMissingOutputProducer
                      : RemediationCause::kNoCompatibleInputSource;
      std::vector<std::string> candidates;
      if (!expected_type.empty() && root.contains("pipeline") &&
          root["pipeline"].is_array()) {
        for (const auto& node : root["pipeline"]) {
          if (!node.is_object()) continue;
          const auto name = node.value("name", "");
          if (name.empty() || name == "input" || name == "output" ||
              name.find('.') != std::string::npos ||
              (diag->path.rfind("/pipeline/", 0) == 0 &&
               name == diag->node_name))
            continue;
          const auto* definition = catalog.FindNode(node.value("type", ""));
          if (!definition) continue;
          for (const auto& output : definition->outputs)
            if (output.type_id == expected_type)
              candidates.push_back(name + "." + output.logical_name);
        }
      }
      if (!expected_type.empty() && io_boundary)
        for (const auto& input : io_boundary->input_published_ports)
          if (input.type_id == expected_type && !input.blackboard_key.empty())
            candidates.push_back(input.blackboard_key);
      std::sort(candidates.begin(), candidates.end());
      candidates.erase(std::unique(candidates.begin(), candidates.end()),
                       candidates.end());
      rem.facts["candidate_sources"] = std::move(candidates);
    }
    diag->remediation = std::move(rem);
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
        rem.cause = RemediationCause::kDuplicateDependency;
        rem.summary =
            "节点 '" + diag->node_name + "' 包含重复依赖 '" + dup_dep + "'。";
        rem.facts["dependency_name"] = dup_dep;
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
        std::string valid_id = item.value("name", "");
        if (!valid_id.empty() && valid_id != diag->node_name) {
          ranked.emplace_back(LevenshteinDistance(dep_id, valid_id), valid_id);
        }
      }
      std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
      });
      std::vector<std::string> candidate_node_names;
      for (const auto& r : ranked) {
        candidate_node_names.push_back(r.second);
      }

      ValidationRemediation rem;
      rem.cause = RemediationCause::kUnknownDependency;
      rem.summary = "节点 '" + diag->node_name + "' 依赖了未知的节点名 '" +
                    dep_id + "'。";
      rem.facts["dependency_name"] = dep_id;
      rem.facts["candidate_node_names"] = candidate_node_names;
      diag->remediation = std::move(rem);
    }
  } else if (diag->code == DiagnosticCode::kPortCardinalityMismatch ||
             diag->code == DiagnosticCode::kPortProvenanceMismatch ||
             diag->code == DiagnosticCode::kPortLifetimeMismatch) {
    // 方向、有效契约和传递形状由 Validator 负责。
    // 路径可指向 Node 输入、其输出或 IO 边界。
    if (diag->facts.empty()) return;
    ValidationRemediation rem;
    rem.cause = RemediationCause::kPortFlowMismatch;
    rem.facts = diag->facts;
    const auto producer_name = rem.facts.value("producer_name", "");
    const auto consumer_name = rem.facts.value("consumer_name", "");
    rem.summary = "生产者 '" + producer_name + "' 与消费者 '" + consumer_name +
                  "' 在端口 '" + diag->port + "' 上的流契约不兼容。";
    if (rem.facts.contains("actual_shape") &&
        rem.facts.contains("expected_shape")) {
      const auto actual = DescribeItemShape(rem.facts["actual_shape"]);
      const auto expected = DescribeItemShape(rem.facts["expected_shape"]);
      if (rem.facts.contains("anchor_port")) {
        rem.summary = "节点 '" + consumer_name + "' 的逐项输入 '" + diag->port +
                      "' 接收" + actual + "，输入 '" +
                      rem.facts["anchor_port"].get<std::string>() + "' 接收" +
                      expected + "，无法逐项配对。";
      } else {
        rem.summary = "生产者 '" + producer_name + "' 在数据 '" +
                      rem.facts.value("bound_key", "") + "' 上提供" + actual +
                      "，消费者 '" + consumer_name + "' 要求" + expected + "。";
      }
    }
    diag->remediation = std::move(rem);
  }
}

}  // namespace

void AttachRemediation(const nlohmann::json& root, ValidationReport* report) {
  if (!report) return;
  const auto catalog = PipelineCatalog::Snapshot();
  for (auto& diag : report->diagnostics) {
    PopulateBasicRemediation(&diag, root, catalog);
  }
}

ValidationReport ValidateWithRemediation(
    const nlohmann::json& root, const PipelineIoBoundary& io_boundary) {
  ValidationReport report = PipelineValidator::Validate(root, io_boundary);
  const auto catalog = PipelineCatalog::Snapshot();
  for (auto& diag : report.diagnostics)
    PopulateBasicRemediation(&diag, root, catalog, &io_boundary);
  return report;
}

ValidationReport ExplainPipeline(const nlohmann::json& root,
                                 const PipelineIoBoundary& io_boundary) {
  return ExplainPipeline(root, ValidateWithRemediation(root, io_boundary),
                         [&](const auto& patched) {
                           return ValidateWithRemediation(patched, io_boundary);
                         });
}

ValidationReport ExplainPipeline(
    const nlohmann::json& root, ValidationReport report,
    const std::function<ValidationReport(const nlohmann::json&)>& revalidate) {
  const auto catalog = PipelineCatalog::Snapshot();
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

    if (diag.remediation->cause == RemediationCause::kUnknownConfigField) {
      const nlohmann::json* config = nullptr;
      std::vector<ConfigFieldDefinition> fields;
      std::string prefix;
      if (diag.path.rfind("/pipeline/", 0) == 0) {
        size_t idx_end = diag.path.find('/', 10);
        size_t p_idx = std::stoul(diag.path.substr(10, idx_end - 10));
        const auto& node_obj = root["pipeline"][p_idx];
        const auto* def = catalog.FindNode(node_obj.value("type", ""));
        if (def && node_obj.contains("params") &&
            node_obj["params"].is_object()) {
          config = &node_obj["params"];
          fields = def->config_fields;
          prefix = "/pipeline/" + std::to_string(p_idx) + "/params/";
        }
      } else if (diag.path.rfind("/models/", 0) == 0 &&
                 diag.remediation->facts.contains("params_path")) {
        size_t idx_end = diag.path.find('/', 8);
        size_t model_index = std::stoul(diag.path.substr(8, idx_end - 8));
        const auto& model = root["models"][model_index];
        const auto& backend = model["backend"];
        const auto params_path =
            diag.remediation->facts["params_path"].get<std::string>();
        const nlohmann::json::json_pointer pointer(params_path);
        if (root.contains(pointer) && root.at(pointer).is_object()) {
          config = &root.at(pointer);
          prefix = params_path + "/";
          if (params_path ==
              "/models/" + std::to_string(model_index) + "/params") {
            const auto implementations =
                ModelRegistry::Instance().FindImplementation(
                    model.value("type", ""), backend.value("type", ""));
            if (implementations.size() == 1)
              fields = implementations.front().params.Fields();
          } else {
            const auto definition =
                PipelineCatalog::FindBackend(backend.value("type", ""));
            if (definition) fields = definition->params.Fields();
          }
        }
      }
      std::string field_name = diag.remediation->facts.value("field", "");
      if (config && config->contains(field_name)) {
        const auto& original_val = config->at(field_name);
        auto candidate_fields = diag.remediation->facts.value(
            "candidate_fields", std::vector<std::string>{});
        for (const auto& cand : candidate_fields) {
          if (config->contains(cand)) continue;
          auto cf_it =
              std::find_if(fields.begin(), fields.end(),
                           [&](const auto& f) { return f.name == cand; });
          if (cf_it != fields.end() &&
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
    } else if (diag.remediation->cause ==
                   RemediationCause::kUnknownModelReference ||
               diag.remediation->cause ==
                   RemediationCause::kModelTypeMismatch) {
      size_t idx_end = diag.path.find('/', 10);
      size_t p_idx = std::stoul(diag.path.substr(10, idx_end - 10));
      const auto& node_obj = root["pipeline"][p_idx];
      std::string node_type = node_obj.value("type", "");
      const auto* def = catalog.FindNode(node_type);
      std::string model_name = diag.remediation->facts.value("model_name", "");
      auto candidate_model_names = diag.remediation->facts.value(
          "candidate_model_names", std::vector<std::string>{});
      if (def) {
        std::string expected_prefix =
            "/pipeline/" + std::to_string(p_idx) + "/params/";
        for (const auto& dep : def->model_dependencies) {
          std::string model_reference_path =
              expected_prefix + EscapeJsonPointer(dep.config_field);
          if (diag.path == model_reference_path) {
            for (const auto& cand_id : candidate_model_names) {
              if (cand_id == model_name) continue;
              ValidationFix fix;
              fix.id = "use-model-" + std::to_string(++fix_counter);
              fix.title = "使用模型 '" + cand_id + "'";
              fix.effect = "将模型引用从 '" + model_name + "' 更改为 '" +
                           cand_id + "'。";
              fix.patch =
                  nlohmann::json::array({{{"op", "test"},
                                          {"path", model_reference_path},
                                          {"value", model_name}},
                                         {{"op", "replace"},
                                          {"path", model_reference_path},
                                          {"value", cand_id}}});
              candidate_fixes.push_back(std::move(fix));
            }
            break;
          }
        }
      }
    } else if (diag.remediation->cause ==
                   RemediationCause::kUnknownNodeReference ||
               diag.remediation->cause ==
                   RemediationCause::kUnknownPortReference ||
               diag.remediation->cause == RemediationCause::kPortTypeMismatch ||
               diag.remediation->cause ==
                   RemediationCause::kNoCompatibleInputSource ||
               diag.remediation->cause ==
                   RemediationCause::kMissingOutputProducer) {
      const auto start = diag.path.rfind("/pipeline/", 0) == 0    ? 10U
                         : diag.path.rfind("/io/output/", 0) == 0 ? 11U
                                                                  : 0U;
      if (!start) continue;
      const auto container_path =
          diag.path.substr(0, diag.path.find('/', start));
      const nlohmann::json::json_pointer pointer(container_path);
      if (!root.contains(pointer)) continue;
      const auto& consumer = root.at(pointer);
      const auto source = diag.remediation->facts.value("bound_key", "");
      std::vector<std::string> candidates;
      const auto dot = source.find('.');
      if (diag.remediation->cause == RemediationCause::kUnknownNodeReference &&
          dot != std::string::npos) {
        for (const auto& name : diag.remediation->facts.value(
                 "candidate_node_names", std::vector<std::string>{}))
          candidates.push_back(name + source.substr(dot));
      } else if (diag.remediation->cause ==
                     RemediationCause::kUnknownPortReference &&
                 dot != std::string::npos) {
        for (const auto& port : diag.remediation->facts.value(
                 "candidate_ports", std::vector<std::string>{}))
          candidates.push_back(source.substr(0, dot + 1) + port);
      } else {
        candidates = diag.remediation->facts.value("candidate_sources",
                                                   std::vector<std::string>{});
      }
      for (const auto& candidate : candidates) {
        if (candidate == source) continue;
        auto inputs = consumer.value("inputs", nlohmann::json::object());
        inputs[diag.port] = candidate;
        ValidationFix fix;
        fix.id = "connect-input-" + std::to_string(++fix_counter);
        fix.title = "将输入 '" + diag.port + "' 连接到 '" + candidate + "'";
        fix.effect = "使用已声明输出 '" + candidate + "' 作为输入来源。";
        fix.patch = nlohmann::json::array(
            {{{"op", "test"}, {"path", container_path}, {"value", consumer}},
             {{"op", "add"},
              {"path", container_path + "/inputs"},
              {"value", std::move(inputs)}}});
        candidate_fixes.push_back(std::move(fix));
      }
    } else if (diag.remediation->cause ==
               RemediationCause::kDuplicateDependency) {
      size_t idx_end = diag.path.find('/', 10);
      size_t consumer_idx = std::stoul(diag.path.substr(10, idx_end - 10));
      const auto& c_node = root["pipeline"][consumer_idx];
      std::string dup_dep =
          diag.remediation->facts.value("dependency_name", "");
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
    } else if (diag.remediation->cause ==
               RemediationCause::kUnknownDependency) {
      size_t idx_end = diag.path.find('/', 10);
      size_t consumer_idx = std::stoul(diag.path.substr(10, idx_end - 10));
      const auto& c_node = root["pipeline"][consumer_idx];
      std::string prefix =
          "/pipeline/" + std::to_string(consumer_idx) + "/depends_on/";
      std::string dep_id = diag.remediation->facts.value("dependency_name", "");
      auto candidate_node_names = diag.remediation->facts.value(
          "candidate_node_names", std::vector<std::string>{});
      if (diag.path.rfind(prefix, 0) == 0 && c_node.contains("depends_on") &&
          c_node["depends_on"].is_array() && !candidate_node_names.empty()) {
        for (const auto& cand_id : candidate_node_names) {
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
      ValidationReport new_report = revalidate(patched_root);
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
            if (diag.remediation->cause ==
                    RemediationCause::kUnknownModelReference ||
                diag.remediation->cause ==
                    RemediationCause::kModelTypeMismatch) {
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
                  if (patched_root["models"][m_idx].value("name", "") ==
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
