#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "company_alg_log.h"
#include "engine/backend_interface.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {

/**
 * @brief 推理后端注册表 (BackendRegistry)
 */
class BackendRegistry {
 public:
  using Creator = std::function<std::unique_ptr<IInferenceBackend>()>;

  static BackendRegistry& Instance();

  bool Register(const BackendDefinition& definition, Creator creator) noexcept;

  std::optional<BackendDefinition> Find(
      const std::string& backend_type) const noexcept;

  std::unique_ptr<IInferenceBackend> Create(
      const std::string& backend_type,
      std::string* diagnostic = nullptr) const noexcept;

  bool Has(const std::string& backend_type) const noexcept;

  std::vector<std::string> ListTypes() const;

  std::vector<BackendDefinition> ListDefinitions() const;

  bool HasConflict() const noexcept;

  std::vector<std::string> GetConflictErrors() const;

  void ClearForTesting();

 private:
  struct Entry {
    BackendDefinition definition;
    Creator creator;
  };

  BackendRegistry() = default;
  void RecordConflict(std::string error) noexcept;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  bool has_conflict_ = false;
  std::vector<std::string> conflict_errors_;
};

#define REGISTER_BACKEND_WITH_DEFINITION(BackendClass, ...)            \
  static bool _registered_backend_##BackendClass = []() noexcept {     \
    try {                                                              \
      const auto definition = (__VA_ARGS__);                           \
      return ::llm_edgeflow::BackendRegistry::Instance().Register(     \
          definition,                                                  \
          []() -> std::unique_ptr<::llm_edgeflow::IInferenceBackend> { \
            return std::make_unique<BackendClass>();                   \
          });                                                          \
    } catch (const std::exception& e) {                                \
      ALG_LOG_ERROR("[BackendRegistry] Failed to register %s: %s\n",   \
                    #BackendClass, e.what());                          \
      return false;                                                    \
    } catch (...) {                                                    \
      ALG_LOG_ERROR("[BackendRegistry] Failed to register %s\n",       \
                    #BackendClass);                                    \
      return false;                                                    \
    }                                                                  \
  }()

}  // namespace llm_edgeflow
