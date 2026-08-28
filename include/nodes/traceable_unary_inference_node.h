#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/traceable_item.h"
#include "nodes/model_bound_node.h"

namespace alg_framework {

template <typename EngineCapability, typename Input, typename Output>
class TraceableUnaryInferenceNode : public ModelBoundNode<EngineCapability> {
 protected:
  using InputBatch = std::vector<TraceableItem<Input>>;
  using OutputBatch = std::vector<TraceableItem<Output>>;

  TraceableUnaryInferenceNode(std::string node_name,
                              std::string default_model_id,
                              std::string input_port_name,
                              std::string output_port_name,
                              int missing_input_error)
      : ModelBoundNode<EngineCapability>(std::move(node_name),
                                         std::move(default_model_id)),
        in_port_(std::move(input_port_name)),
        out_port_(std::move(output_port_name)),
        missing_input_error_(missing_input_error) {}

  TraceableUnaryInferenceNode(std::string node_name,
                              std::string default_model_id,
                              const BlackboardKey<InputBatch>& input_key,
                              const BlackboardKey<OutputBatch>& output_key,
                              int missing_input_error)
      : ModelBoundNode<EngineCapability>(std::move(node_name),
                                         std::move(default_model_id)),
        in_port_(input_key.name),
        out_port_(output_key.name),
        missing_input_error_(missing_input_error) {}

  virtual int InferBatch(const InputBatch& input, OutputBatch* output) = 0;

  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& /*config*/,
                     SessionContext& /*session_ctx*/) override {
    this->BindPort(init_ctx, in_port_);
    this->BindPort(init_ctx, out_port_);
    return true;
  }

 private:
  int ProcessNode(AlgContext& req_ctx) final {
    const auto* inputs = in_port_.Require(req_ctx, missing_input_error_);
    if (!inputs) {
      return missing_input_error_;
    }
    OutputBatch outputs;
    int ret = InferBatch(*inputs, &outputs);
    if (ret != 0) {
      return this->Fail(req_ctx, ret, this->Name() + " inference failed");
    }
    out_port_.Set(req_ctx, std::move(outputs));
    return 0;
  }

  BoundInput<InputBatch> in_port_;
  BoundOutput<OutputBatch> out_port_;
  const int missing_input_error_;
};

}  // namespace alg_framework
