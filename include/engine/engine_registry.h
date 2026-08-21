#pragma once

#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/engine_interface.h"

namespace alg_framework {

class EngineFactory {
 public:
  using CreatorFunc = std::function<std::unique_ptr<IModelEngine>()>;

  static EngineFactory& Instance() {
    static EngineFactory instance;
    return instance;
  }

  bool Register(const std::string& engine_type, CreatorFunc creator) noexcept {
    try {
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

  bool HasConflict() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_conflict_;
  }

  std::vector<std::string> GetConflictErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return conflict_errors_;
  }

 private:
  EngineFactory() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CreatorFunc> creators_;
  bool has_conflict_ = false;
  std::vector<std::string> conflict_errors_;
};

#define REGISTER_ENGINE(EngineTypeStr, ClassName)                \
  static bool _registered_engine_##ClassName = []() noexcept {   \
    return ::alg_framework::EngineFactory::Instance().Register(  \
        EngineTypeStr,                                           \
        []() -> std::unique_ptr<::alg_framework::IModelEngine> { \
          return std::make_unique<ClassName>();                  \
        });                                                      \
  }()

}  // namespace alg_framework
