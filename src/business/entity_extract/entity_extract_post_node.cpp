#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "business/entity_extract/entity_extract_contract.h"
#include "business/entity_extract/entity_extract_dto.h"
#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 实体提取后处理算子 (解析 0.6B LLM 输出并打包输出领域 DTO)
 */
class EntityExtractPostNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "EntityExtractPostNode";

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* req_ids = req_ctx->Get(kRawRequestIds);
    auto* llm_answers = req_ctx->Get(kGeneratedLlmAnswers);

    if (!req_ids || !llm_answers) {
      req_ctx->SetError(-6101, "EntityExtractPostNode: Missing inputs");
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

    req_ctx->Set(kEntityExtractOutputs, std::move(outputs));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = kNodeType;
    return name;
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
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(EntityExtractPostNode,
                              MakeEntityExtractPostNodeDefinition());

}  // namespace alg_framework
