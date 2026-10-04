#include "pipeline_remediation.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "contracts/json_pointer.h"
#include "core/name_suggestions.h"
#include "core/pipeline_catalog.h"

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

void PopulateBasicRemediation(ValidationDiagnostic* diag,
                              const nlohmann::json& root,
                              const PipelineCatalogSnapshot& catalog) {
  if (!diag || diag->remediation.has_value()) return;
  if (!root.is_object()) return;

  if (diag->code == DiagnosticCode::kUnknownNodeType ||
      diag->code == DiagnosticCode::kUnknownModelType ||
      diag->code == DiagnosticCode::kUnknownBackend) {
    const nlohmann::json::json_pointer pointer(diag->path);
    if (!root.contains(pointer) || !root.at(pointer).is_string()) return;
    const auto& item = root.at(nlohmann::json::json_pointer(
        diag->path.substr(0, diag->path.rfind('/'))));
    const std::string name = root.at(pointer).get<std::string>();
    ValidationRemediation rem;
    std::vector<std::string> names;
    std::string candidates_key;
    std::string next_step;
    if (diag->code == DiagnosticCode::kUnknownNodeType) {
      rem.cause = RemediationCause::kUnknownNodeType;
      rem.facts["node_type"] = name;
      candidates_key = "candidate_node_types";
      rem.summary = "节点 '" + diag->node_id + "' 的 node_type '" + name;
      next_step = "新增的 Node 需要重新构建后才会注册。";
    } else {
      const std::string model_id = item.value("model_id", "");
      rem.facts["model_id"] = model_id;
      if (diag->code == DiagnosticCode::kUnknownModelType) {
        rem.cause = RemediationCause::kUnknownModelType;
        rem.facts["model_type"] = name;
        candidates_key = "candidate_model_types";
        for (const auto& def : PipelineCatalog::Models())
          names.push_back(def.model_type);
        std::sort(names.begin(), names.end());
        rem.facts["registered_model_types"] = names;
        rem.summary = "模型 '" + model_id + "' 的 model_type '" + name;
      } else {
        rem.cause = RemediationCause::kUnknownBackend;
        rem.facts["backend"] = name;
        candidates_key = "candidate_backends";
        for (const auto& def : PipelineCatalog::Backends())
          names.push_back(def.backend_type);
        std::sort(names.begin(), names.end());
        rem.facts["registered_backends"] = names;
        rem.summary = "模型 '" + model_id + "' 的 backend '" + name;
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
            rem.cause = RemediationCause::kUnknownConfigField;
            rem.summary = "节点 '" + diag->node_id + "' 的配置包含未知字段 '" +
                          field_name + "'。";
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
            rem.cause = RemediationCause::kMissingConfigField;
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
            rem.cause = RemediationCause::kInvalidConfigValue;
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
          if (def) {
            std::string expected_prefix =
                "/pipeline/" + std::to_string(p_idx) + "/config/";
            for (const auto& dep : def->model_dependencies) {
              if (diag->path !=
                  expected_prefix + EscapeJsonPointer(dep.config_field)) {
                continue;
              }
              std::string model_id =
                  (node_obj.contains("config") &&
                   node_obj["config"].contains(dep.config_field) &&
                   node_obj["config"][dep.config_field].is_string())
                      ? node_obj["config"][dep.config_field].get<std::string>()
                      : "";
              std::string req_cap = dep.capability;

              ValidationRemediation rem;
              rem.schema_version = 1;
              rem.cause = (diag->code == DiagnosticCode::kUnknownModelReference)
                              ? RemediationCause::kUnknownModelReference
                              : RemediationCause::kModelCapabilityMismatch;
              rem.facts["model_id"] = model_id;
              rem.facts["required_capability"] = req_cap;

              std::vector<std::string> candidate_model_ids;
              if (root.contains("models") && root["models"].is_array()) {
                for (size_t m_idx = 0; m_idx < root["models"].size(); ++m_idx) {
                  const auto& m = root["models"][m_idx];
                  if (!m.is_object()) continue;
                  std::string mid = m.value("model_id", "");
                  if (mid.empty()) continue;
                  const auto model_def =
                      PipelineCatalog::FindModel(m.value("model_type", ""));
                  if (model_def && model_def->capability == req_cap) {
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
              break;
            }
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
      std::string consumer_type = consumer_node.value("node_type", "");
      const auto* consumer_def = catalog.FindNode(consumer_type);
      std::string port_name = diag->port;
      std::string bound_key;
      if (consumer_node.contains("inputs") &&
          consumer_node["inputs"].contains(port_name)) {
        bound_key = consumer_node["inputs"][port_name].get<std::string>();
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
          if (p_node.contains("outputs") &&
              p_node["outputs"].contains(out.logical_name)) {
            actual_out_key =
                p_node["outputs"][out.logical_name].get<std::string>();
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

      if (producer_idx >= 0 && producer_out_type != expected_type) {
        ValidationRemediation rem;
        rem.schema_version = 1;
        rem.cause = RemediationCause::kPortTypeMismatch;
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
      } else {
        ValidationRemediation rem;
        rem.schema_version = 1;
        rem.cause = RemediationCause::kNoCompatibleInputSource;
        rem.summary =
            bound_key.empty()
                ? "请为必需输入端口 '" + port_name + "' 明确指定数据来源。"
                : "Pipeline 中没有为端口 '" + port_name + "' (绑定键: '" +
                      bound_key + "') 提供唯一匹配类型的生产者。";
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
        rem.cause = RemediationCause::kDuplicateDependency;
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
      rem.cause = RemediationCause::kUnknownDependency;
      rem.summary =
          "节点 '" + diag->node_id + "' 依赖了未知的节点 ID '" + dep_id + "'。";
      rem.facts["dependency_id"] = dep_id;
      rem.facts["candidate_node_ids"] = candidate_node_ids;
      diag->remediation = std::move(rem);
    }
  } else if (diag->code == DiagnosticCode::kMissingBizOutput) {
    ValidationRemediation rem;
    rem.schema_version = 1;
    rem.cause = RemediationCause::kMissingBizOutput;
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
      std::string bound_key;
      if (consumer_node.contains("inputs") &&
          consumer_node["inputs"].contains(port_name)) {
        bound_key = consumer_node["inputs"][port_name].get<std::string>();
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
                if (p_node.contains("outputs") &&
                    p_node["outputs"].contains(out.logical_name)) {
                  actual_out_key =
                      p_node["outputs"][out.logical_name].get<std::string>();
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
      rem.cause = RemediationCause::kPortFlowMismatch;
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

}  // namespace

void AttachRemediation(const nlohmann::json& root, ValidationReport* report) {
  if (!report) return;
  const auto catalog = PipelineCatalog::Snapshot();
  for (auto& diag : report->diagnostics) {
    PopulateBasicRemediation(&diag, root, catalog);
  }
}

ValidationReport ValidateWithRemediation(
    const nlohmann::json& root, const PipelineIoBoundary* io_boundary) {
  ValidationReport report = PipelineValidator::Validate(root, io_boundary);
  AttachRemediation(root, &report);
  return report;
}

ValidationReport ExplainPipeline(const nlohmann::json& root,
                                 const PipelineIoBoundary* io_boundary) {
  const auto catalog = PipelineCatalog::Snapshot();
  ValidationReport report = ValidateWithRemediation(root, io_boundary);
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
    } else if (diag.remediation->cause ==
                   RemediationCause::kUnknownModelReference ||
               diag.remediation->cause ==
                   RemediationCause::kModelCapabilityMismatch) {
      size_t idx_end = diag.path.find('/', 10);
      size_t p_idx = std::stoul(diag.path.substr(10, idx_end - 10));
      const auto& node_obj = root["pipeline"][p_idx];
      std::string node_type = node_obj.value("node_type", "");
      const auto* def = catalog.FindNode(node_type);
      std::string model_id = diag.remediation->facts.value("model_id", "");
      auto candidate_model_ids = diag.remediation->facts.value(
          "candidate_model_ids", std::vector<std::string>{});
      if (def) {
        std::string expected_prefix =
            "/pipeline/" + std::to_string(p_idx) + "/config/";
        for (const auto& dep : def->model_dependencies) {
          std::string model_path =
              expected_prefix + EscapeJsonPointer(dep.config_field);
          if (diag.path == model_path) {
            for (const auto& cand_id : candidate_model_ids) {
              if (cand_id == model_id) continue;
              ValidationFix fix;
              fix.id = "use-model-" + std::to_string(++fix_counter);
              fix.title = "使用模型 '" + cand_id + "'";
              fix.effect =
                  "将模型引用从 '" + model_id + "' 更改为 '" + cand_id + "'。";
              fix.patch = nlohmann::json::array(
                  {{{"op", "test"}, {"path", model_path}, {"value", model_id}},
                   {{"op", "replace"},
                    {"path", model_path},
                    {"value", cand_id}}});
              candidate_fixes.push_back(std::move(fix));
            }
            break;
          }
        }
      }
    } else if (diag.remediation->cause ==
               RemediationCause::kDuplicateDependency) {
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
    } else if (diag.remediation->cause ==
               RemediationCause::kUnknownDependency) {
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
          ValidateWithRemediation(patched_root, io_boundary);
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
                    RemediationCause::kModelCapabilityMismatch) {
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
