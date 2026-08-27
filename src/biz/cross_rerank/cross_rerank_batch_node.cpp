#include <iostream>
#include <string>
#include <vector>

#include "biz/cross_rerank/cross_rerank_contract.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "engine/engine_interface.h"
#include "nodes/traceable_unary_inference_node.h"

namespace alg_framework {

class CrossRerankBatchNode final
    : public TraceableUnaryInferenceNode<IRerankEngine,
                                         IRerankEngine::PairInput, float> {
 public:
  inline static constexpr char kNodeType[] = "CrossRerankBatchNode";

  CrossRerankBatchNode()
      : TraceableUnaryInferenceNode(kNodeType, "rerank_model_v1",
                                    kRerankPairItems, kRerankScoredItems,
                                    -7201) {}

 protected:
  int InferBatch(const InputBatch& input, OutputBatch* output) override {
    return engine()->ScoreTraceableBatch(input, output);
  }
};

NodeDefinition MakeCrossRerankBatchNodeDefinition() {
  NodeDefinition def;
  def.node_type = CrossRerankBatchNode::kNodeType;
  def.category = "biz";
  def.description = "Cross-encoder batch reranking scoring node";
  def.inputs = {RequiredInput(kRerankPairItems)};
  def.outputs = {Output(kRerankScoredItems)};
  def.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "rerank_model_v1"}};
  def.model_capability = "rerank";
  def.model_config_field = "bind_model";
  def.biz_names = {kCrossRerankBizName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(CrossRerankBatchNode,
                              MakeCrossRerankBatchNodeDefinition());

}  // namespace alg_framework
