#pragma once

#include <memory>
#include <string>
#include <utility>

#include "core/pipeline_catalog.h"
#include "nodes/node_support.h"

namespace llm_edgeflow {

template <typename ModelCapability>
class ModelBoundNode : public NodeBase {
 public:
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
    const NodeDefinition* definition = PipelineCatalog::FindNode(Name());
    if (!definition || definition->model_config_field.empty()) return false;

    const std::string& field_name = definition->model_config_field;
    model_id_.clear();
    if (config.contains(field_name) && config[field_name].is_string()) {
      model_id_ = config[field_name].get<std::string>();
    }
    if (model_id_.empty()) {
      for (const auto& field : definition->config_fields) {
        if (field.name == field_name && field.default_value.is_string()) {
          model_id_ = field.default_value.get<std::string>();
          break;
        }
      }
    }
    if (model_id_.empty()) return false;
    model_ = session_ctx.GetModelManager().GetModel<ModelCapability>(model_id_);
    if (!model_) {
      return false;
    }
    return InitModelNode(init_ctx, config, session_ctx);
  }

  std::string model_id_;
  std::shared_ptr<ModelCapability> model_;
};

}  // namespace llm_edgeflow
