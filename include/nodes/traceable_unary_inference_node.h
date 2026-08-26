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
                              const BlackboardKey<InputBatch>& input_key,
                              const BlackboardKey<OutputBatch>& output_key,
                              int missing_input_error)
      : ModelBoundNode<EngineCapability>(std::move(node_name),
                                         std::move(default_model_id)),
        input_key_(input_key),
        output_key_(output_key),
        missing_input_error_(missing_input_error) {}

  virtual int InferBatch(const InputBatch& input, OutputBatch* output) = 0;

 private:
  int ProcessNode(AlgContext& req_ctx) final {
    const auto* inputs =
        this->Require(req_ctx, input_key_, missing_input_error_);
    if (!inputs) {
      return missing_input_error_;
    }
    OutputBatch outputs;
    int ret = InferBatch(*inputs, &outputs);
    if (ret != 0) {
      return this->Fail(req_ctx, ret, this->Name() + " inference failed");
    }
    this->Publish(req_ctx, output_key_, std::move(outputs));
    return 0;
  }

  const BlackboardKey<InputBatch>& input_key_;
  const BlackboardKey<OutputBatch>& output_key_;
  const int missing_input_error_;
};

}  // namespace alg_framework
