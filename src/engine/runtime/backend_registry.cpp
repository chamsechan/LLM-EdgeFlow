#include "engine/backend_registry.h"

#include <algorithm>
#include <exception>
#include <unordered_set>
#include <utility>

#include "contracts/config_schema_validation.h"
#include "engine/runtime/registry_support.h"

namespace alg_framework {

BackendRegistry& BackendRegistry::Instance() {
  static BackendRegistry instance;
  return instance;
}

void BackendRegistry::RecordConflict(std::string error) noexcept {
  registry_support::RecordConflict(mutex_, has_conflict_, conflict_errors_,
                                   std::move(error));
}

bool BackendRegistry::Register(const BackendDefinition& definition,
                               Creator creator) noexcept {
  try {
    if (definition.backend_type.empty()) {
      RecordConflict("Backend registration failed: empty backend_type");
      return false;
    }
    if (definition.supported_protocols.empty()) {
      RecordConflict(
          "Backend registration failed: empty supported_protocols in " +
          definition.backend_type);
      return false;
    }
    if (!IsValidInferenceConcurrency(definition.concurrency)) {
      RecordConflict("Backend registration failed: invalid concurrency in " +
                     definition.backend_type);
      return false;
    }

    std::unordered_set<int> seen_protocols;
    for (ExecutionProtocol protocol : definition.supported_protocols) {
      if (!IsValidExecutionProtocol(protocol)) {
        RecordConflict("Backend registration failed: invalid protocol in " +
                       definition.backend_type);
        return false;
      }
      if (!seen_protocols.insert(static_cast<int>(protocol)).second) {
        RecordConflict("Backend registration failed: duplicate protocol in " +
                       definition.backend_type);
        return false;
      }
    }

    if (!creator) {
      RecordConflict("Backend registration failed: null creator for " +
                     definition.backend_type);
      return false;
    }

    std::string schema_error;
    if (!ValidateConfigFieldDefinitions(definition.config_fields,
                                        &schema_error)) {
      RecordConflict("Backend " + definition.backend_type +
                     " schema validation failed: " + schema_error);
      return false;
    }

    Entry staged{definition, std::move(creator)};
    std::lock_guard<std::mutex> lock(mutex_);
    const bool inserted =
        entries_.emplace(definition.backend_type, std::move(staged)).second;
    if (!inserted) {
      has_conflict_ = true;
      try {
        conflict_errors_.push_back("Duplicate backend registration for type: " +
                                   definition.backend_type);
      } catch (...) {
      }
      ALG_LOG_ERROR("[BackendRegistry] Duplicate backend registration: %s\n",
                    definition.backend_type.c_str());
      return false;
    }
    return true;
  } catch (const std::exception& error) {
    try {
      RecordConflict("Exception registering backend " +
                     definition.backend_type + ": " + error.what());
    } catch (...) {
      RecordConflict("Exception registering backend");
    }
    return false;
  } catch (...) {
    RecordConflict("Unknown exception registering backend");
    return false;
  }
}

std::optional<BackendDefinition> BackendRegistry::Find(
    const std::string& backend_type) const noexcept {
  return registry_support::FindDefinition<BackendDefinition>(mutex_, entries_,
                                                             backend_type);
}

std::unique_ptr<IInferenceBackend> BackendRegistry::Create(
    const std::string& backend_type, std::string* diagnostic) const noexcept {
  try {
    Creator creator;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = entries_.find(backend_type);
      if (it == entries_.end()) {
        registry_support::SetDiagnostic(
            diagnostic, "Unknown backend type: " + backend_type);
        return nullptr;
      }
      creator = it->second.creator;
    }

    try {
      return creator();
    } catch (const std::exception& error) {
      registry_support::SetDiagnostic(
          diagnostic,
          "Exception creating backend " + backend_type + ": " + error.what());
      return nullptr;
    } catch (...) {
      registry_support::SetDiagnostic(
          diagnostic, "Unknown exception creating backend " + backend_type);
      return nullptr;
    }
  } catch (const std::exception& error) {
    try {
      registry_support::SetDiagnostic(
          diagnostic,
          "Exception preparing backend " + backend_type + ": " + error.what());
    } catch (...) {
      registry_support::SetDiagnostic(diagnostic,
                                      "Exception preparing backend");
    }
    return nullptr;
  } catch (...) {
    registry_support::SetDiagnostic(diagnostic,
                                    "Unknown exception preparing backend");
    return nullptr;
  }
}

bool BackendRegistry::Has(const std::string& backend_type) const noexcept {
  return registry_support::HasType(mutex_, entries_, backend_type);
}

std::vector<std::string> BackendRegistry::ListTypes() const {
  return registry_support::ListTypes(mutex_, entries_);
}

std::vector<BackendDefinition> BackendRegistry::ListDefinitions() const {
  return registry_support::ListDefinitions<BackendDefinition>(
      mutex_, entries_, [](const auto& lhs, const auto& rhs) {
        return lhs.backend_type < rhs.backend_type;
      });
}

bool BackendRegistry::HasConflict() const noexcept {
  return registry_support::HasConflict(mutex_, has_conflict_);
}

std::vector<std::string> BackendRegistry::GetConflictErrors() const {
  return registry_support::GetConflictErrors(mutex_, conflict_errors_);
}

void BackendRegistry::ClearForTesting() {
  registry_support::Clear(mutex_, entries_, has_conflict_, conflict_errors_);
}

}  // namespace alg_framework
