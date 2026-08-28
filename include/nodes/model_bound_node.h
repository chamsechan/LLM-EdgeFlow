#pragma once

#include <memory>
#include <string>
#include <utility>

#include "nodes/node_support.h"

namespace alg_framework {

template <typename EngineCapability>
class ModelBoundNode : public NodeBase {
 public:
  explicit ModelBoundNode(std::string node_name,
                          std::string default_model_id = "default_model")
      : NodeBase(std::move(node_name)),
        default_model_id_(std::move(default_model_id)) {}

 protected:
  const std::shared_ptr<EngineCapability>& engine() const noexcept {
    return engine_;
  }

  virtual bool InitModelNode(const NodeInitContext& init_ctx,
                             const nlohmann::json& config,
                             SessionContext& session_ctx) {
    (void)init_ctx;
    return InitModelNode(config, session_ctx);
  }

  virtual bool InitModelNode(const nlohmann::json& /*config*/,
                             SessionContext& /*session_ctx*/) {
    return true;
  }

 private:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& session_ctx) final {
    std::string model_id = config.value("bind_model", default_model_id_);
    if (model_id.empty()) {
      model_id = default_model_id_;
    }
    engine_ =
        session_ctx.GetModelManager().GetModel<EngineCapability>(model_id);
    if (!engine_) {
      return false;
    }
    return InitModelNode(init_ctx, config, session_ctx);
  }

  const std::string default_model_id_;
  std::shared_ptr<EngineCapability> engine_;
};

}  // namespace alg_framework
