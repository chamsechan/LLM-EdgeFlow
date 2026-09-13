#pragma once

#include <memory>
#include <string>
#include <utility>

#include "contracts/config_schema_validation.h"
#include "core/pipeline_catalog.h"
#include "engine/model_capability_traits.h"
#include "nodes/node_base.h"

namespace llm_edgeflow {

template <typename ModelCapability>
class ModelBoundNode : public NodeBase {
 public:
  using ModelInterface = ModelCapability;

  explicit ModelBoundNode(std::string node_name)
      : NodeBase(std::move(node_name)) {}

 protected:
  const std::shared_ptr<ModelCapability>& model() const noexcept {
    return model_;
  }

  const std::string& model_id() const noexcept { return model_id_; }

  virtual bool InitModelNode(const NodeInitContext& init_ctx,
                             const nlohmann::json& config,
                             SessionContext& session_ctx) {
    (void)init_ctx;
    (void)config;
    (void)session_ctx;
    return true;
  }

 private:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& session_ctx) final {
    const auto definition = PipelineCatalog::FindNode(Name());
    if (!definition || definition->model_dependencies.size() != 1) {
      return init_ctx.Fail(
          "Node Definition must declare exactly one model dependency");
    }
    const auto& dep = definition->model_dependencies.front();
    if (dep.capability !=
        ModelCapabilityTraits<ModelCapability>::Capability()) {
      return init_ctx.Fail(
          "Node model capability does not match template capability");
    }

    nlohmann::json normalized;
    if (init_ctx.plan) {
      const auto* binding = init_ctx.plan->FindModelBinding(dep.name);
      if (!binding || binding->model_id.empty()) {
        return init_ctx.Fail("Model binding for '" + dep.name +
                             "' is missing or empty in plan");
      }
      if (binding->capability != dep.capability) {
        return init_ctx.Fail("Model binding capability mismatch for '" +
                             dep.name + "'");
      }
      model_id_ = binding->model_id;
      normalized = init_ctx.plan->normalized_config;
    } else {
      std::vector<ConfigFieldValidationError> errors;
      if (!ValidateAndNormalizeFields(definition->config_fields, config,
                                      &normalized, &errors)) {
        return init_ctx.Fail(errors.empty() ? "Invalid model Node configuration"
                                            : errors.front().message);
      }
      model_id_.clear();
      if (normalized.contains(dep.config_field) &&
          normalized[dep.config_field].is_string()) {
        model_id_ = normalized[dep.config_field].template get<std::string>();
      }
      if (model_id_.empty()) {
        return init_ctx.Fail("Model binding field '" + dep.config_field +
                             "' is empty");
      }
    }

    model_ = session_ctx.GetModelManager().GetModel<ModelCapability>(model_id_);
    if (!model_) {
      return init_ctx.Fail(
          "Model '" + model_id_ +
          "' is unavailable or has an incompatible capability");
    }
    return InitModelNode(init_ctx, normalized, session_ctx);
  }

  std::string model_id_;
  std::shared_ptr<ModelCapability> model_;
};

}  // namespace llm_edgeflow
