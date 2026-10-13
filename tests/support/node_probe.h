#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"

namespace llm_edgeflow::test_support {

struct NodeBinding {
  std::string name;
  std::string key;
  std::string lifetime{};
};

// Algorithm tests supply explicit keys and consume the production declaration.
// Validator tests separately own topology, boundary closure and flow
// validation.
struct NodeProbe {
  std::string diagnostic;
  ValidatedNodePlan plan;
  std::unique_ptr<INode> node;

  bool Init(const std::string& type, const nlohmann::json& params,
            SessionContext& session, const std::vector<NodeBinding>& inputs,
            const std::vector<NodeBinding>& outputs) {
    node.reset();
    plan = {};
    diagnostic.clear();
    const auto definition = NodeRegistry::Instance().Find(type);
    if (!definition) {
      diagnostic = "Unregistered Node: " + type;
      return false;
    }
    std::vector<ConfigFieldValidationError> errors;
    if (!ValidateAndNormalizeFields(definition->config_fields, params,
                                    &plan.normalized_params, &errors)) {
      if (!errors.empty())
        diagnostic = errors.front().path + ": " + errors.front().message;
      return false;
    }
    plan.node.name = type;
    plan.node.node_type = type;
    const auto bind = [&](const std::vector<NodeBinding>& bindings,
                          const std::vector<NodePortDefinition>& declarations,
                          PortDirection direction) {
      for (const auto& binding : bindings) {
        const auto port = std::find_if(
            declarations.begin(), declarations.end(), [&](const auto& item) {
              return item.logical_name == binding.name;
            });
        if (port == declarations.end()) {
          diagnostic = "Undeclared port: " + binding.name;
          return false;
        }
        std::string lifetime =
            binding.lifetime.empty() ? port->lifetime : binding.lifetime;
        if (direction == PortDirection::kOutput && binding.lifetime.empty() &&
            !port->lifetime_from_input.empty()) {
          const auto* anchor = plan.FindPort(port->lifetime_from_input);
          if (anchor) lifetime = anchor->lifetime;
        }
        plan.ports.push_back({binding.name, binding.key, port->type_id,
                              port->cardinality, port->provenance_policy,
                              std::move(lifetime), direction});
      }
      return true;
    };
    if (!bind(inputs, definition->inputs, PortDirection::kInput) ||
        !bind(outputs, definition->outputs, PortDirection::kOutput))
      return false;
    for (const auto& dependency : definition->model_dependencies) {
      plan.model_bindings.push_back(
          {dependency.name, dependency.model_type, dependency.config_field,
           plan.normalized_params.at(dependency.config_field)
               .get<std::string>()});
    }
    node = NodeRegistry::Instance().Create(type);
    if (!node) {
      diagnostic = "Node creator returned null: " + type;
      return false;
    }
    return node->Init({&plan, &session, &diagnostic});
  }
};

inline bool RegisterNodeModel(SessionContext& session, std::string name,
                              std::shared_ptr<IModel> model) {
  ModelRegistration registration;
  registration.model_name = std::move(name);
  registration.impl_name = model->ImplName();
  registration.model_type = model->ModelType();
  registration.model = std::move(model);
  return session.GetModelManager().RegisterBatch({std::move(registration)});
}

}  // namespace llm_edgeflow::test_support
