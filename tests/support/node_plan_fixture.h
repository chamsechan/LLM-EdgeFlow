#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
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
    std::string* error,
    const std::unordered_map<std::string, std::string>& input_lifetimes = {}) {
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
      *error = field_errors.front().path + ": " + field_errors.front().message;
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
    const std::string impl_name = "node_fixture_model_" + dep.model_type;
    const std::string backend = "node_fixture_backend_" + dep.model_type;
    if (!ModelRegistry::Instance().Has(impl_name)) {
      ModelDefinition model;
      model.impl_name = impl_name;
      model.model_type = dep.model_type;
      model.required_protocol = ExecutionProtocol::kFixture;
      model.fixture_backends = {backend};
      model.concurrency = InferenceConcurrency::kConcurrent;
      ModelRegistry::Instance().Register(
          model, [](const auto&, auto*) { return nullptr; });
    }
    if (!BackendRegistry::Instance().Has(backend)) {
      BackendDefinition implementation;
      implementation.backend_type = backend;
      implementation.supported_protocols = {ExecutionProtocol::kFixture};
      implementation.concurrency = InferenceConcurrency::kConcurrent;
      BackendRegistry::Instance().Register(implementation,
                                           [] { return nullptr; });
    }
    models.push_back({{"name", id},
                      {"type", dep.model_type},
                      {"backend", {{"type", backend}}},
                      {"file", "mock.bin"}});
  }
  PipelineIoBoundary boundary;
  nlohmann::json inputs = nlohmann::json::object();
  for (const auto& port : definition->inputs) {
    if (omitted.count(port.logical_name)) continue;
    IoPortDefinition external;
    static_cast<PortContract&>(external) = port;
    external.blackboard_key = "input." + port.logical_name;
    const auto life = input_lifetimes.find(port.logical_name);
    if (life != input_lifetimes.end()) external.lifetime = life->second;
    inputs[port.logical_name] = external.blackboard_key;
    boundary.input_published_ports.push_back(std::move(external));
  }
  for (const auto& port : definition->outputs) {
    IoPortDefinition consumer;
    static_cast<PortContract&>(consumer) = port;
    consumer.blackboard_key = "fixture_node." + port.logical_name;
    consumer.lifetime_from_input.clear();
    consumer.logical_name = port.logical_name;
    boundary.output_consumed_ports.push_back(std::move(consumer));
  }
  nlohmann::json node = {{"name", "fixture_node"},
                         {"type", type},
                         {"params", config},
                         {"inputs", std::move(inputs)}};
  auto result = PipelineValidator::ValidateAndPlan(
      {{"models", std::move(models)},
       {"pipeline", nlohmann::json::array({std::move(node)})}},
      boundary);
  if (!result.report.ok) {
    if (error) *error = result.report.diagnostics.front().message;
    return nullptr;
  }
  if (error) error->clear();
  auto node_plan = std::make_shared<ValidatedNodePlan>(
      std::move(result.node_plans.at("fixture_node")));
  // Unit callers choose local context keys; the graph above uses canonical
  // references.
  for (auto& port : node_plan->ports)
    port.blackboard_key =
        (port.direction == PortDirection::kInput ? input_prefix
                                                 : output_prefix) +
        port.logical_name;
  return node_plan;
}

}  // namespace llm_edgeflow
