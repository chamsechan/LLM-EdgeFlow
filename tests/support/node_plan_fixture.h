#pragma once

#include <memory>
#include <mutex>
#include <unordered_set>

#include "contracts/config_schema_validation.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

inline std::shared_ptr<ValidatedNodePlan> PrepareNodePlanForTest(
    const std::string& type, const nlohmann::json& config,
    const std::unordered_set<std::string>& omitted,
    const std::string& input_prefix, const std::string& output_prefix,
    std::string* error) {
  static std::mutex fixture_mutex;
  std::lock_guard<std::mutex> fixture_lock(fixture_mutex);
  const auto definition = PipelineCatalog::FindNode(type);
  if (!definition) {
    if (error) *error = "Missing NodeDefinition: " + type;
    return nullptr;
  }
  nlohmann::json normalized;
  std::vector<ConfigFieldValidationError> field_errors;
  if (!ValidateAndNormalizeFields(definition->config_fields, config,
                                  &normalized, &field_errors)) {
    if (error)
      *error =
          field_errors.front().field_name + ": " + field_errors.front().message;
    return nullptr;
  }
  nlohmann::json models = nlohmann::json::array();
  std::unordered_set<std::string> declared_models;
  for (const auto& dep : definition->model_dependencies) {
    if (!normalized.contains(dep.config_field) ||
        !normalized[dep.config_field].is_string())
      continue;
    const std::string id = normalized[dep.config_field].get<std::string>();
    if (!declared_models.insert(id).second) continue;
    const std::string model_type = "node_fixture_model_" + dep.capability;
    const std::string backend = "node_fixture_backend_" + dep.capability;
    if (!ModelRegistry::Instance().Has(model_type)) {
      ModelDefinition model;
      model.model_type = model_type;
      model.capability = dep.capability;
      model.required_protocol = ExecutionProtocol::kTensorGraph;
      model.concurrency = InferenceConcurrency::kConcurrent;
      ModelRegistry::Instance().Register(
          model, [](const auto&, auto*) { return nullptr; });
    }
    if (!BackendRegistry::Instance().Has(backend)) {
      BackendDefinition implementation;
      implementation.backend_type = backend;
      implementation.supported_protocols = {ExecutionProtocol::kTensorGraph};
      implementation.concurrency = InferenceConcurrency::kConcurrent;
      BackendRegistry::Instance().Register(implementation,
                                           [] { return nullptr; });
    }
    models.push_back({{"model_id", id},
                      {"model_type", model_type},
                      {"backend", backend},
                      {"model_path", "mock.bin"}});
  }
  BizDefinition biz{"fixture_" + input_prefix + output_prefix + type};
  nlohmann::json inputs = nlohmann::json::object();
  nlohmann::json outputs = nlohmann::json::object();
  auto bind = [&](const NodePortDefinition& port, const std::string& prefix,
                  std::vector<BizPortDefinition>* boundary,
                  nlohmann::json* bindings) {
    BizPortDefinition external;
    static_cast<PortContract&>(external) = port;
    external.blackboard_key = prefix + port.logical_name;
    if (!port.lifetime_config_field.empty()) {
      external.lifetime =
          normalized.value(port.lifetime_config_field, port.lifetime);
      external.lifetime_config_field.clear();
      biz.biz_name += "_" + port.logical_name + "_" + external.lifetime;
    }
    (*bindings)[port.logical_name] = external.blackboard_key;
    boundary->push_back(std::move(external));
  };
  for (const auto& port : definition->inputs) {
    if (omitted.count(port.logical_name)) {
      biz.biz_name += "_omit_" + port.logical_name;
    } else {
      bind(port, input_prefix, &biz.ingress, &inputs);
    }
  }
  for (const auto& port : definition->outputs)
    bind(port, output_prefix, &biz.egress, &outputs);
  if (!PipelineCatalog::FindBiz(biz.biz_name) &&
      !PipelineCatalog::RegisterBizDefinition(biz)) {
    if (error) *error = "Failed to register Node fixture business";
    return nullptr;
  }
  nlohmann::json node = {{"id", "fixture_node"},
                         {"node_type", type},
                         {"depends_on", nlohmann::json::array()},
                         {"config", config},
                         {"inputs", std::move(inputs)},
                         {"outputs", std::move(outputs)}};
  auto result = PipelineValidator::ValidateAndPlan(
      {{"biz_name", biz.biz_name},
       {"models", std::move(models)},
       {"pipeline", nlohmann::json::array({std::move(node)})}});
  if (!result.report.ok) {
    if (error) *error = result.report.diagnostics.front().message;
    return nullptr;
  }
  if (error) error->clear();
  return std::make_shared<ValidatedNodePlan>(
      std::move(result.node_plans.at("fixture_node")));
}

}  // namespace llm_edgeflow
