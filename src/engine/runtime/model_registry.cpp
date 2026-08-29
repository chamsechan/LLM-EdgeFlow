#include "engine/model_registry.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "contracts/config_schema_validation.h"

namespace alg_framework {

namespace {

void SetDiagnostic(std::string* diagnostic,
                   const std::string& message) noexcept {
  if (!diagnostic) return;
  try {
    *diagnostic = message;
  } catch (...) {
  }
}

}  // namespace

ModelRegistry& ModelRegistry::Instance() {
  static ModelRegistry instance;
  return instance;
}

void ModelRegistry::RecordConflict(std::string error) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    has_conflict_ = true;
    try {
      conflict_errors_.push_back(std::move(error));
    } catch (...) {
    }
  } catch (...) {
  }
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
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(model_type);
    if (it == entries_.end()) return std::nullopt;
    return it->second.definition;
  } catch (...) {
    return std::nullopt;
  }
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
        SetDiagnostic(diagnostic, "Unknown model type: " + model_type);
        return nullptr;
      }
      creator = it->second.creator;
    }

    try {
      return creator(context, diagnostic);
    } catch (const std::exception& error) {
      SetDiagnostic(diagnostic, "Exception creating model " + model_type +
                                    ": " + error.what());
      return nullptr;
    } catch (...) {
      SetDiagnostic(diagnostic,
                    "Unknown exception creating model " + model_type);
      return nullptr;
    }
  } catch (const std::exception& error) {
    try {
      SetDiagnostic(diagnostic, "Exception preparing model " + model_type +
                                    ": " + error.what());
    } catch (...) {
      SetDiagnostic(diagnostic, "Exception preparing model");
    }
    return nullptr;
  } catch (...) {
    SetDiagnostic(diagnostic, "Unknown exception preparing model");
    return nullptr;
  }
}

bool ModelRegistry::Has(const std::string& model_type) const noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(model_type) != entries_.end();
  } catch (...) {
    return false;
  }
}

std::vector<std::string> ModelRegistry::ListTypes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  result.reserve(entries_.size());
  for (const auto& item : entries_) result.push_back(item.first);
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<ModelDefinition> ModelRegistry::ListDefinitions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ModelDefinition> result;
  result.reserve(entries_.size());
  for (const auto& item : entries_) result.push_back(item.second.definition);
  std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.model_type < rhs.model_type;
  });
  return result;
}

bool ModelRegistry::HasConflict() const noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_conflict_;
  } catch (...) {
    return true;
  }
}

std::vector<std::string> ModelRegistry::GetConflictErrors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return conflict_errors_;
}

void ModelRegistry::ClearForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  has_conflict_ = false;
  conflict_errors_.clear();
}

}  // namespace alg_framework
