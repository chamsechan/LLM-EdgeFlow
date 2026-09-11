#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/node_definition.h"
#include "engine/model_capability_traits.h"

namespace llm_edgeflow {

/**
 * @brief 为单模型 Custom Node 快速构造 NodeDefinition
 * 自动填充 Node 类型、custom 类别、bind_model 字段和 ModelCapabilityTraits
 * 对应的模型能力， 默认使用 parallel_safe = false。
 */
template <typename NodeT>
inline NodeDefinition MakeCustomModelNodeDefinition(
    std::string description, std::vector<NodePortDefinition> inputs,
    std::vector<NodePortDefinition> outputs) {
  using ModelType = typename NodeT::ModelInterface;
  NodeDefinition def;
  def.node_type = NodeT::kNodeType;
  def.category = "custom";
  def.description = std::move(description);
  def.inputs = std::move(inputs);
  def.outputs = std::move(outputs);
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, true}};
  def.model_capability = ModelCapabilityTraits<ModelType>::Capability();
  def.model_config_field = "bind_model";
  def.parallel_safe = false;
  return def;
}

}  // namespace llm_edgeflow
