#pragma once

#include <memory>
#include <string>
#include <utility>

#include "core/pipeline_catalog.h"
#include "engine/model_capability_traits.h"
#include "nodes/model_binding.h"
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

    std::string error;
    if (!ResolveBoundModelId(*init_ctx.plan, dep.name, dep.capability,
                             &model_id_, &error)) {
      return init_ctx.Fail(error);
    }

    model_ = session_ctx.GetModelManager().GetModel<ModelCapability>(model_id_);
    if (!model_) {
      return init_ctx.Fail(
          "Model '" + model_id_ +
          "' is unavailable or has an incompatible capability");
    }
    return InitModelNode(init_ctx, config, session_ctx);
  }

  std::string model_id_;
  std::shared_ptr<ModelCapability> model_;
};

}  // namespace llm_edgeflow
