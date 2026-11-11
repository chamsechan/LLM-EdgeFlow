#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "core/node_definition.h"

namespace llm_edgeflow {

const std::unordered_set<std::string>& ValidCardinalities();
const std::unordered_set<std::string>& ValidProvenance();
const std::unordered_set<std::string>& ValidLifetimes();

/**
 * @brief 校验一组端口的名称、类型及流属性声明；遇到首个错误即返回 false。
 * 非空 seen_keys 会累积已检查的名称，用于拒绝重复端口；失败时不回滚该集合。
 */
template <typename Port>
bool ValidatePortDefinitions(const std::vector<Port>& ports,
                             std::unordered_set<std::string>* seen_keys,
                             std::string* error) {
  for (const auto& port : ports) {
    if (port.Name().empty()) {
      if (error) *error = "Port key cannot be empty";
      return false;
    }
    if (port.type_id.empty()) {
      if (error)
        *error = "Port type_id cannot be empty for port: " + port.Name();
      return false;
    }
    if (!ValidCardinalities().count(port.cardinality)) {
      if (error) {
        *error = "Invalid port cardinality '" + port.cardinality +
                 "' in port: " + port.Name();
      }
      return false;
    }
    if (!ValidProvenance().count(port.provenance_policy)) {
      if (error) {
        *error = "Invalid port provenance policy '" + port.provenance_policy +
                 "' in port: " + port.Name();
      }
      return false;
    }
    if (!ValidLifetimes().count(port.lifetime)) {
      if (error) {
        *error = "Invalid port lifetime '" + port.lifetime +
                 "' in port: " + port.Name();
      }
      return false;
    }
    if (seen_keys && !seen_keys->insert(port.Name()).second) {
      if (error) *error = "Duplicate port key: " + port.Name();
      return false;
    }
  }
  return true;
}

/**
 * @brief 校验 NodeDefinition 声明的结构一致性，供 Node 注册阶段使用。
 * 检查端口、组合约束、Control schema、配置字段及模型依赖声明；
 * 实际 Pipeline 的连线和模型绑定由 PipelineValidator 校验。
 * @param definition 待校验的 Node 声明。
 * @param error 可选的诊断输出，成功时清空，失败时写入原因。
 * @return true 声明通过结构校验，false 声明不合法。
 */
bool ValidateNodeDefinitionStructure(const NodeDefinition& definition,
                                     std::string* error = nullptr);

}  // namespace llm_edgeflow
