#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "business/entity_extract/entity_extract_contract.h"
#include "business/entity_extract/entity_extract_dto.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"
#include "nodes/node_support.h"

namespace alg_framework {

/**
 * @brief 实体提取后处理算子 (解析 0.6B LLM 输出并打包输出领域 DTO)
 */
class EntityExtractPostNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "EntityExtractPostNode";

  EntityExtractPostNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* req_ids = Require(req_ctx, kRawRequestIds, -6101);
    const auto* llm_answers = Require(req_ctx, kGeneratedLlmAnswers, -6101);

    if (!req_ids || !llm_answers) {
      return -6101;
    }

    size_t batch_size = req_ids->size();
    std::vector<EntityExtractResult> outputs(batch_size);

    for (size_t i = 0; i < batch_size; ++i) {
      outputs[i].request_id = (*req_ids)[i];
      outputs[i].status_code = 0;
      outputs[i].entities_json = "{\"nouns\":[]}";
    }

    // 根据 req_id 精准对齐
    for (const auto& item : *llm_answers) {
      uint32_t r_id = item.req_id;
      if (r_id < batch_size) {
        outputs[r_id].entities_json = item.data;
      }
    }

    std::cout << "[EntityExtractPostNode] Packaged " << batch_size
              << " entity extraction outputs." << std::endl;

    Publish(req_ctx, kEntityExtractOutputs, std::move(outputs));
    return 0;
  }
};

NodeDefinition MakeEntityExtractPostNodeDefinition() {
  NodeDefinition def;
  def.node_type = EntityExtractPostNode::kNodeType;
  def.category = "business";
  def.description = "Entity extract post-processing node";
  def.inputs = {RequiredInput(kRawRequestIds),
                RequiredInput(kGeneratedLlmAnswers)};
  def.outputs = {Output(kEntityExtractOutputs)};
  def.business_names = {kEntityExtractBusinessName,
                        kEntityExtractLlamaCppBusinessName};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(EntityExtractPostNode,
                              MakeEntityExtractPostNodeDefinition());

}  // namespace alg_framework
