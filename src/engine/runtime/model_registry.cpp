#include "engine/model_registry.h"

#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <utility>

#include "contracts/config_schema_validation.h"
#include "contracts/diagnostic.h"
#include "engine/backend_registry.h"
#include "engine/runtime/registry_support.h"

namespace llm_edgeflow {
namespace {
bool Compatible(const ModelDefinition& model,
                const BackendDefinition& backend) {
  if (std::find(backend.supported_protocols.begin(),
                backend.supported_protocols.end(),
                model.required_protocol) == backend.supported_protocols.end())
    return false;
  return model.required_protocol != ExecutionProtocol::kFixture ||
         std::find(model.fixture_backends.begin(), model.fixture_backends.end(),
                   backend.backend_type) != model.fixture_backends.end();
}
}  // namespace

ModelRegistry& ModelRegistry::Instance() {
  static ModelRegistry instance;
  return instance;
}

void ModelRegistry::RecordConflict(std::string_view error) noexcept {
  registry_support::RecordConflict(mutex_, conflicts_, std::move(error));
}

bool ModelRegistry::Register(const ModelDefinition& definition,
                             Creator creator) noexcept {
  try {
    if (definition.impl_name.empty()) {
      RecordConflict("Model registration failed: empty impl_name");
      return false;
    }
    if (definition.model_type.empty()) {
      RecordConflict("Model registration failed: empty model_type in " +
                     definition.impl_name);
      return false;
    }
    if (!IsValidExecutionProtocol(definition.required_protocol)) {
      RecordConflict("Model registration failed: invalid protocol in " +
                     definition.impl_name);
      return false;
    }
    if (!IsValidInferenceConcurrency(definition.concurrency)) {
      RecordConflict("Model registration failed: invalid concurrency in " +
                     definition.impl_name);
      return false;
    }
    if (!creator) {
      RecordConflict("Model registration failed: null creator for " +
                     definition.impl_name);
      return false;
    }
    if ((definition.required_protocol == ExecutionProtocol::kFixture) !=
        !definition.fixture_backends.empty()) {
      RecordConflict("Only fixture models must declare fixture_backends: " +
                     definition.impl_name);
      return false;
    }
    std::set<std::string> fixture_names;
    for (const auto& backend : definition.fixture_backends) {
      if (backend.empty() || !fixture_names.insert(backend).second) {
        RecordConflict("Invalid fixture_backends in " + definition.impl_name);
        return false;
      }
    }

    std::string schema_error;
    if (!ValidateConfigFieldDefinitions(definition.params.Fields(),
                                        &schema_error)) {
      RecordConflict("Model " + definition.impl_name +
                     " schema validation failed: " + schema_error);
      return false;
    }

    Entry staged{definition, std::move(creator)};
    std::lock_guard<std::mutex> lock(mutex_);
    const bool inserted =
        entries_.emplace(definition.impl_name, std::move(staged)).second;
    if (!inserted) {
      try {
        conflicts_.Record("Duplicate model registration for type: " +
                          definition.impl_name);
      } catch (...) {
        conflicts_.Record(std::string());
      }
      ALG_LOG_ERROR("[ModelRegistry] Duplicate model registration: %s\n",
                    definition.impl_name.c_str());
      return false;
    }
    return true;
  } catch (const std::exception& error) {
    try {
      RecordConflict("Exception registering model " + definition.impl_name +
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
    const std::string& impl_name) const noexcept {
  return registry_support::FindDefinition<ModelDefinition>(mutex_, entries_,
                                                           impl_name);
}

std::shared_ptr<IModel> ModelRegistry::Create(
    const std::string& impl_name, const ModelCreateContext& context,
    std::string* diagnostic) const noexcept {
  try {
    Creator creator;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = entries_.find(impl_name);
      if (it == entries_.end()) {
        SetDiagnosticNoexcept(diagnostic, "Unknown model type: " + impl_name);
        return nullptr;
      }
      creator = it->second.creator;
    }

    try {
      return creator(context, diagnostic);
    } catch (const std::exception& error) {
      SetDiagnosticNoexcept(diagnostic, "Exception creating model " +
                                            impl_name + ": " + error.what());
      return nullptr;
    } catch (...) {
      SetDiagnosticNoexcept(diagnostic,
                            "Unknown exception creating model " + impl_name);
      return nullptr;
    }
  } catch (const std::exception& error) {
    try {
      SetDiagnosticNoexcept(diagnostic, "Exception preparing model " +
                                            impl_name + ": " + error.what());
    } catch (...) {
      SetDiagnosticNoexcept(diagnostic, "Exception preparing model");
    }
    return nullptr;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown exception preparing model");
    return nullptr;
  }
}

bool ModelRegistry::Has(const std::string& impl_name) const noexcept {
  return registry_support::HasType(mutex_, entries_, impl_name);
}

std::vector<std::string> ModelRegistry::ListImplNames() const {
  return registry_support::ListTypes(mutex_, entries_);
}

std::vector<ModelDefinition> ModelRegistry::ListDefinitions() const {
  return registry_support::ListDefinitions<ModelDefinition>(
      mutex_, entries_, [](const auto& lhs, const auto& rhs) {
        return lhs.impl_name < rhs.impl_name;
      });
}

std::vector<ModelDefinition> ModelRegistry::FindImplementation(
    const std::string& model_type, const std::string& backend_type) const {
  const auto backend = BackendRegistry::Instance().Find(backend_type);
  std::vector<ModelDefinition> selected;
  if (!backend) return selected;
  for (const auto& model : ListDefinitions())
    if (model.model_type == model_type && Compatible(model, *backend))
      selected.push_back(model);
  return selected;
}

bool ModelRegistry::Audit(const BackendRegistry& registry,
                          std::vector<std::string>* errors) const {
  std::vector<std::string> failures;
  const auto models = ListDefinitions();
  const auto backends = registry.ListDefinitions();
  std::map<std::pair<std::string, std::string>, std::vector<std::string>>
      selections;
  for (const auto& model : models) {
    for (const auto& backend : backends) {
      if (Compatible(model, backend))
        selections[{model.model_type, backend.backend_type}].push_back(
            model.impl_name);
    }
  }
  for (const auto& [pair, implementations] : selections) {
    if (implementations.size() < 2) continue;
    auto message =
        "Multiple implementations for " + pair.first + "/" + pair.second + ":";
    for (const auto& name : implementations) message += " " + name;
    failures.push_back(std::move(message));
  }
  if (errors) *errors = failures;
  return failures.empty();
}

bool ModelRegistry::HasConflict() const noexcept {
  return registry_support::HasConflict(mutex_, conflicts_);
}

std::vector<std::string> ModelRegistry::GetConflictErrors() const {
  return registry_support::GetConflictErrors(mutex_, conflicts_);
}

}  // namespace llm_edgeflow
