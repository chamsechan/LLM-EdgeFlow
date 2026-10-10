#pragma once

#include <any>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "engine/model_interface.h"

namespace llm_edgeflow {

template <typename T>
class SessionResourceKey {
 public:
  explicit SessionResourceKey(std::string name) : name_(std::move(name)) {
    if (name_.empty()) {
      throw std::invalid_argument("Session resource key must not be empty");
    }
  }

  const std::string& Name() const noexcept { return name_; }

 private:
  std::string name_;
};

/**
 * @brief 句柄级运行时配置与资源参数
 */
struct RuntimeOptions {
  int device_id = -1;  // -1 表示未指定/默认，>=0 表示物理设备 ID
  bool has_device_id = false;
  std::string chip_type = "UNKNOWN";
};

/**
 * @brief 模型会话级注册元数据
 */
struct ModelRegistration {
  std::string model_name;
  std::string impl_name;
  std::string model_type;
  std::string backend_type;
  std::string revision;
  std::shared_ptr<IModel> model;
  std::string model_file;
  nlohmann::json model_params = nlohmann::json::object();
  nlohmann::json backend_params = nlohmann::json::object();
};

/**
 * @brief 单句柄持有的模型实例资源池 (ModelManager)
 */
class ModelManager {
 public:
  bool RegisterBatch(const std::vector<ModelRegistration>& models) {
    if (models.empty()) return true;

    std::unordered_set<std::string> staged_ids;
    std::vector<ModelRegistration> staged_registrations;
    staged_registrations.reserve(models.size());
    for (const auto& item : models) {
      if (item.model_name.empty() || !item.model) {
        return false;
      }
      if (!staged_ids.insert(item.model_name).second) {
        return false;  // staging 内重复
      }
      // 核对注册元数据与模型自身身份一致性
      if (!item.impl_name.empty() && item.model->ImplName() != item.impl_name) {
        return false;
      }
      if (!item.model_type.empty() &&
          item.model->ModelType() != item.model_type) {
        return false;
      }
      std::string rev = item.revision;
      if (rev.empty()) {
        if (item.impl_name.empty() || item.backend_type.empty() ||
            item.model_file.empty() || !item.model_params.is_object() ||
            !item.backend_params.is_object()) {
          return false;
        }
        rev = item.impl_name + "\n" + item.backend_type + "\n" +
              item.model_file + "\n" + item.model_params.dump() + "\n" +
              item.backend_params.dump();
      }
      ModelRegistration reg = item;
      reg.revision = std::move(rev);
      staged_registrations.push_back(std::move(reg));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : staged_registrations) {
      if (registrations_.find(item.model_name) != registrations_.end()) {
        return false;
      }
    }

    // 所有可能失败的准备工作在临时容器完成，最终通过 noexcept swap 提交。
    auto new_registrations = registrations_;

    for (auto& item : staged_registrations) {
      const std::string model_name = item.model_name;
      new_registrations[model_name] = std::move(item);
    }
    registrations_.swap(new_registrations);
    return true;
  }

  template <typename T>
  std::shared_ptr<T> GetModel(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(model_name);
    if (it != registrations_.end()) {
      auto res = std::dynamic_pointer_cast<T>(it->second.model);
      if (res) return res;
    }
    return nullptr;
  }

  bool HasModel(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registrations_.find(model_name) != registrations_.end();
  }

  std::string GetModelRevision(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(model_name);
    return it == registrations_.end() ? std::string() : it->second.revision;
  }

  std::optional<ModelRegistration> GetModelRegistration(
      const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(model_name);
    if (it == registrations_.end()) return std::nullopt;
    return it->second;
  }

  bool UpdateModelRevision(const std::string& model_name,
                           std::string revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(model_name);
    if (revision.empty() || it == registrations_.end()) {
      return false;
    }
    it->second.revision = std::move(revision);
    return true;
  }

  std::unordered_map<std::string, std::shared_ptr<IModel>> GetAllModels()
      const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, std::shared_ptr<IModel>> result;
    result.reserve(registrations_.size());
    for (const auto& pair : registrations_) {
      result.emplace(pair.first, pair.second.model);
    }
    return result;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, ModelRegistration> registrations_;
};

/**
 * @brief 句柄级会话上下文 (SessionContext)
 */
class SessionContext {
 public:
  SessionContext() = default;
  ~SessionContext() = default;

  ModelManager& GetModelManager() { return model_manager_; }
  const ModelManager& GetModelManager() const { return model_manager_; }

  void SetRuntimeOptions(const RuntimeOptions& options) {
    runtime_options_ = options;
  }
  const RuntimeOptions& GetRuntimeOptions() const { return runtime_options_; }

  template <typename T>
  void SetResource(const SessionResourceKey<T>& key,
                   std::shared_ptr<T> resource) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    auto it = resources_.find(key.Name());
    if (it != resources_.end()) {
      RequireResourceType<T>(key.Name(), it->second.type);
    }
    auto flight = flights_.find(key.Name());
    if (flight != flights_.end()) {
      RequireResourceType<T>(key.Name(), flight->second->type);
    }
    resources_.insert_or_assign(
        key.Name(),
        ResourceEntry{std::move(resource), std::type_index(typeid(T))});
  }

  template <typename T>
  std::shared_ptr<T> GetResource(const SessionResourceKey<T>& key) const {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    auto it = resources_.find(key.Name());
    if (it == resources_.end()) return nullptr;
    RequireResourceType<T>(key.Name(), it->second.type);
    return std::static_pointer_cast<T>(it->second.value);
  }

  template <typename T, typename FactoryFunc>
  std::shared_ptr<T> GetOrCreateResource(const SessionResourceKey<T>& key,
                                         FactoryFunc&& factory) {
    std::shared_ptr<SingleFlightEntry> flight;
    {
      std::lock_guard<std::mutex> lock(resource_mutex_);
      auto it = resources_.find(key.Name());
      if (it != resources_.end()) {
        RequireResourceType<T>(key.Name(), it->second.type);
        return std::static_pointer_cast<T>(it->second.value);
      }
      auto fit = flights_.find(key.Name());
      if (fit == flights_.end()) {
        flight =
            std::make_shared<SingleFlightEntry>(std::type_index(typeid(T)));
        flights_[key.Name()] = flight;
      } else {
        flight = fit->second;
        RequireResourceType<T>(key.Name(), flight->type);
      }
    }

    std::lock_guard<std::mutex> key_lock(flight->mtx);
    if (flight->done) {
      RequireResourceType<T>(key.Name(), flight->type);
      if (flight->failure) std::rethrow_exception(flight->failure);
      return std::static_pointer_cast<T>(flight->result);
    }

    std::shared_ptr<T> created;
    try {
      created = factory();
      if (created) {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        resources_.insert_or_assign(
            key.Name(), ResourceEntry{created, std::type_index(typeid(T))});
      }
    } catch (...) {
      // 将本次尝试的失败共享给等待者，但不缓存。
      flight->failure = std::current_exception();
      created.reset();
    }
    flight->result = created;
    flight->done = true;

    {
      std::lock_guard<std::mutex> lock(resource_mutex_);
      flights_.erase(key.Name());
    }
    if (flight->failure) std::rethrow_exception(flight->failure);
    return created;
  }

 private:
  struct ResourceEntry {
    std::shared_ptr<void> value;
    std::type_index type;
  };

  struct SingleFlightEntry {
    explicit SingleFlightEntry(std::type_index resource_type)
        : type(resource_type) {}

    std::mutex mtx;
    std::shared_ptr<void> result;
    std::exception_ptr failure;
    std::type_index type;
    bool done = false;
  };

  template <typename T>
  static void RequireResourceType(const std::string& key,
                                  std::type_index actual) {
    const std::type_index expected(typeid(T));
    if (actual != expected) {
      throw std::logic_error("Session resource type mismatch for key '" + key +
                             "' (expected " + expected.name() + ", stored " +
                             actual.name() + ")");
    }
  }

  ModelManager model_manager_;
  mutable std::mutex resource_mutex_;
  std::unordered_map<std::string, ResourceEntry> resources_;
  std::unordered_map<std::string, std::shared_ptr<SingleFlightEntry>> flights_;
  RuntimeOptions runtime_options_;
};

}  // namespace llm_edgeflow
