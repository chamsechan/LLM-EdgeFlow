#include "engine/model_registry.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "contracts/config_schema_validation.h"
#include "engine/runtime/registry_support.h"

namespace llm_edgeflow {

ModelRegistry& ModelRegistry::Instance() {
  static ModelRegistry instance;
  return instance;
}

void ModelRegistry::RecordConflict(std::string error) noexcept {
  registry_support::RecordConflict(mutex_, has_conflict_, conflict_errors_,
                                   std::move(error));
}

bool ModelRegistry::Register(const ModelDefinition& definition,
                             Creator creator) noexcept {
  try {
    if (definition.model_type.empty()) {
      RecordConflict("Model registration failed: empty model_type");
      return false;
    }
    if (definition.capability.empty()) {
      RecordConflict("Model registration failed: empty capability in " +
                     definition.model_type);
      return false;
    }
    if (!IsValidExecutionProtocol(definition.required_protocol)) {
      RecordConflict("Model registration failed: invalid protocol in " +
                     definition.model_type);
      return false;
    }
    if (!IsValidInferenceConcurrency(definition.concurrency)) {
      RecordConflict("Model registration failed: invalid concurrency in " +
                     definition.model_type);
      return false;
    }
    if (!creator) {
      RecordConflict("Model registration failed: null creator for " +
                     definition.model_type);
      return false;
    }

    std::string schema_error;
    if (!ValidateConfigFieldDefinitions(definition.config_fields,
                                        &schema_error)) {
      RecordConflict("Model " + definition.model_type +
                     " schema validation failed: " + schema_error);
      return false;
    }

    Entry staged{definition, std::move(creator)};
    std::lock_guard<std::mutex> lock(mutex_);
    const bool inserted =
        entries_.emplace(definition.model_type, std::move(staged)).second;
    if (!inserted) {
      has_conflict_ = true;
      try {
        conflict_errors_.push_back("Duplicate model registration for type: " +
                                   definition.model_type);
      } catch (...) {
      }
      ALG_LOG_ERROR("[ModelRegistry] Duplicate model registration: %s\n",
                    definition.model_type.c_str());
      return false;
    }
    return true;
  } catch (const std::exception& error) {
    try {
      RecordConflict("Exception registering model " + definition.model_type +
                     ": " + error.what());
    } catch (...) {
      RecordConflict("Exception registering model");
    }
    return false;
  } catch (...) {
    RecordConflict("Unknown exception registering model");
    return false;
  }
}

std::optional<ModelDefinition> ModelRegistry::Find(
    const std::string& model_type) const noexcept {
  return registry_support::FindDefinition<ModelDefinition>(mutex_, entries_,
                                                           model_type);
}

std::shared_ptr<IModel> ModelRegistry::Create(
    const std::string& model_type, const ModelCreateContext& context,
    std::string* diagnostic) const noexcept {
  try {
    Creator creator;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = entries_.find(model_type);
      if (it == entries_.end()) {
        registry_support::SetDiagnostic(diagnostic,
                                        "Unknown model type: " + model_type);
        return nullptr;
      }
      creator = it->second.creator;
    }

    try {
      return creator(context, diagnostic);
    } catch (const std::exception& error) {
      registry_support::SetDiagnostic(
          diagnostic,
          "Exception creating model " + model_type + ": " + error.what());
      return nullptr;
    } catch (...) {
      registry_support::SetDiagnostic(
          diagnostic, "Unknown exception creating model " + model_type);
      return nullptr;
    }
  } catch (const std::exception& error) {
    try {
      registry_support::SetDiagnostic(
          diagnostic,
          "Exception preparing model " + model_type + ": " + error.what());
    } catch (...) {
      registry_support::SetDiagnostic(diagnostic, "Exception preparing model");
    }
    return nullptr;
  } catch (...) {
    registry_support::SetDiagnostic(diagnostic,
                                    "Unknown exception preparing model");
    return nullptr;
  }
}

bool ModelRegistry::Has(const std::string& model_type) const noexcept {
  return registry_support::HasType(mutex_, entries_, model_type);
}

std::vector<std::string> ModelRegistry::ListTypes() const {
  return registry_support::ListTypes(mutex_, entries_);
}

std::vector<ModelDefinition> ModelRegistry::ListDefinitions() const {
  return registry_support::ListDefinitions<ModelDefinition>(
      mutex_, entries_, [](const auto& lhs, const auto& rhs) {
        return lhs.model_type < rhs.model_type;
      });
}

bool ModelRegistry::HasConflict() const noexcept {
  return registry_support::HasConflict(mutex_, has_conflict_);
}

std::vector<std::string> ModelRegistry::GetConflictErrors() const {
  return registry_support::GetConflictErrors(mutex_, conflict_errors_);
}

void ModelRegistry::ClearForTesting() {
  registry_support::Clear(mutex_, entries_, has_conflict_, conflict_errors_);
}

}  // namespace llm_edgeflow
