#pragma once

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/pipeline_catalog.h"
#include "engine/engine_interface.h"

namespace alg_framework {

class EngineFactory {
 public:
  using CreatorFunc = std::function<std::unique_ptr<IModelEngine>()>;

  static EngineFactory& Instance() {
    static EngineFactory instance;
    return instance;
  }

  bool Register(const std::string& engine_type, CreatorFunc creator,
                const EngineDefinition* definition = nullptr) noexcept {
    try {
      if (definition == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        has_conflict_ = true;
        conflict_errors_.push_back(
            "Engine registration requires a valid Definition: " + engine_type);
        return false;
      }
      if (definition->engine_type != engine_type) {
        std::lock_guard<std::mutex> lock(mutex_);
        has_conflict_ = true;
        conflict_errors_.push_back(
            "EngineDefinition engine_type mismatch: expected " + engine_type +
            ", got " + definition->engine_type);
        return false;
      }
      if (engine_type.empty() || !creator) {
        std::lock_guard<std::mutex> lock(mutex_);
        has_conflict_ = true;
        conflict_errors_.push_back(
            "Empty engine_type or null creator function");
        return false;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = creators_.find(engine_type);
      if (it != creators_.end()) {
        has_conflict_ = true;
        conflict_errors_.push_back("Duplicate engine registration for type: " +
                                   engine_type);
        std::cerr << "[EngineFactory ERROR] Duplicate engine registration: "
                  << engine_type << std::endl;
        return false;
      }
      if (!PipelineCatalog::RegisterEngineDefinition(*definition)) {
        has_conflict_ = true;
        conflict_errors_.push_back("Invalid or duplicate engine Definition: " +
                                   engine_type);
        return false;
      }
      creators_[engine_type] = std::move(creator);
      return true;
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(mutex_);
      has_conflict_ = true;
      conflict_errors_.push_back("Exception registering engine " + engine_type +
                                 ": " + e.what());
      return false;
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      has_conflict_ = true;
      conflict_errors_.push_back("Unknown exception registering engine " +
                                 engine_type);
      return false;
    }
  }

  bool Register(const std::string& engine_type, CreatorFunc creator,
                const EngineDefinition& definition) noexcept {
    return Register(engine_type, std::move(creator), &definition);
  }

  std::unique_ptr<IModelEngine> Create(const std::string& engine_type) const {
    CreatorFunc creator;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = creators_.find(engine_type);
      if (it == creators_.end()) return nullptr;
      creator = it->second;
    }
    // R1-ACC-004: 锁外执行外部 creator，避免嵌套查询或构造导致自锁
    return creator();
  }

  bool Has(const std::string& engine_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return creators_.find(engine_type) != creators_.end();
  }

  std::vector<std::string> ListTypes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(creators_.size());
    for (const auto& item : creators_) result.push_back(item.first);
    std::sort(result.begin(), result.end());
    return result;
  }

  bool HasConflict() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_conflict_;
  }

  std::vector<std::string> GetConflictErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return conflict_errors_;
  }

  void ClearForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    creators_.clear();
    has_conflict_ = false;
    conflict_errors_.clear();
  }

 private:
  EngineFactory() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CreatorFunc> creators_;
  bool has_conflict_ = false;
  std::vector<std::string> conflict_errors_;
};

#define REGISTER_ENGINE_WITH_DEFINITION(EngineTypeStr, ClassName, ...) \
  static bool _registered_engine_##ClassName = []() noexcept {         \
    const auto definition = (__VA_ARGS__);                             \
    return ::alg_framework::EngineFactory::Instance().Register(        \
        EngineTypeStr,                                                 \
        []() -> std::unique_ptr<::alg_framework::IModelEngine> {       \
          return std::make_unique<ClassName>();                        \
        },                                                             \
        &definition);                                                  \
  }()

#define REGISTER_ENGINE(EngineTypeStr, ClassName)                        \
  static_assert(false,                                                   \
                "REGISTER_ENGINE without definition is deprecated. Use " \
                "REGISTER_ENGINE_WITH_DEFINITION instead.")

}  // namespace alg_framework
