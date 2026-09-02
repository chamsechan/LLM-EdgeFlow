#pragma once

#include <any>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "engine/model_interface.h"

namespace llm_edgeflow {

/**
 * @brief 句柄级运行时配置与资源参数
 */
struct RuntimeOptions {
  int device_id = -1;  // -1 表示未指定/默认，>=0 表示物理设备 ID
  bool has_device_id = false;
  int biz_type = 0;
  std::string biz_name;
  std::string chip_type = "UNKNOWN";
  int platform_max_batch = 1;
  uint32_t depth_num = 1;
};

/**
 * @brief 模型会话级注册元数据
 */
struct ModelRegistration {
  std::string model_id;
  std::string model_type;
  std::string capability;
  std::string backend_type;
  std::string revision;
  std::shared_ptr<IModel> model;
  std::string resolved_model_path;
  nlohmann::json normalized_model_config = nlohmann::json::object();
  nlohmann::json normalized_backend_config = nlohmann::json::object();
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
      if (item.model_id.empty() || !item.model) {
        return false;
      }
      if (!staged_ids.insert(item.model_id).second) {
        return false;  // staging 内重复
      }
      // 核对注册元数据与模型自身身份一致性
      if (!item.model_type.empty() &&
          item.model->ModelType() != item.model_type) {
        return false;
      }
      if (!item.capability.empty() &&
          item.model->Capability() != item.capability) {
        return false;
      }
      std::string rev = item.revision;
      if (rev.empty()) {
        if (item.model_type.empty() || item.backend_type.empty() ||
            item.resolved_model_path.empty() ||
            !item.normalized_model_config.is_object() ||
            !item.normalized_backend_config.is_object()) {
          return false;
        }
        rev = item.model_type + "\n" + item.backend_type + "\n" +
              item.resolved_model_path + "\n" +
              item.normalized_model_config.dump() + "\n" +
              item.normalized_backend_config.dump();
      }
      ModelRegistration reg = item;
      reg.revision = std::move(rev);
      staged_registrations.push_back(std::move(reg));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : staged_registrations) {
      if (registrations_.find(item.model_id) != registrations_.end()) {
        return false;
      }
    }

    // 所有可能失败的准备工作在临时容器完成，最终通过 noexcept swap 提交。
    auto new_registrations = registrations_;

    for (auto& item : staged_registrations) {
      const std::string model_id = item.model_id;
      new_registrations[model_id] = std::move(item);
    }
    registrations_.swap(new_registrations);
    return true;
  }

  /**
   * @brief 注册单个模型实例
   */
  bool RegisterModel(const std::string& model_id, std::shared_ptr<IModel> model,
                     std::string revision = {}, std::string model_type = {},
                     std::string capability = {},
                     std::string backend_type = {}) {
    if (model_id.empty() || !model) return false;
    ModelRegistration reg;
    reg.model_id = model_id;
    reg.model_type = std::move(model_type);
    reg.capability = std::move(capability);
    reg.backend_type = std::move(backend_type);
    reg.revision = std::move(revision);
    reg.model = std::move(model);
    return RegisterBatch({std::move(reg)});
  }

  template <typename T>
  std::shared_ptr<T> GetModel(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(model_id);
    if (it != registrations_.end()) {
      auto res = std::dynamic_pointer_cast<T>(it->second.model);
      if (res) return res;
    }
    return nullptr;
  }

  bool HasModel(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registrations_.find(model_id) != registrations_.end();
  }

  std::string GetModelRevision(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(model_id);
    return it == registrations_.end() ? std::string() : it->second.revision;
  }

  std::optional<ModelRegistration> GetModelRegistration(
      const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(model_id);
    if (it == registrations_.end()) return std::nullopt;
    return it->second;
  }

  bool UpdateModelRevision(const std::string& model_id, std::string revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registrations_.find(model_id);
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

  std::vector<ModelRegistration> GetAllRegistrations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModelRegistration> result;
    result.reserve(registrations_.size());
    for (const auto& pair : registrations_) {
      result.push_back(pair.second);
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

  const std::string& GetChipType() const { return runtime_options_.chip_type; }
  int GetPlatformMaxBatch() const {
    return runtime_options_.platform_max_batch;
  }
  uint32_t GetDepthNum() const { return runtime_options_.depth_num; }

  template <typename T>
  void SetResource(const std::string& key, std::shared_ptr<T> resource) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    resources_[key] = resource;
  }

  template <typename T>
  std::shared_ptr<T> GetResource(const std::string& key) const {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    auto it = resources_.find(key);
    if (it == resources_.end()) return nullptr;
    return std::static_pointer_cast<T>(it->second);
  }

  template <typename T, typename FactoryFunc>
  std::shared_ptr<T> GetOrCreateResource(const std::string& key,
                                         FactoryFunc&& factory) {
    std::shared_ptr<SingleFlightEntry> flight;
    {
      std::lock_guard<std::mutex> lock(resource_mutex_);
      auto it = resources_.find(key);
      if (it != resources_.end()) {
        return std::static_pointer_cast<T>(it->second);
      }
      auto fit = flights_.find(key);
      if (fit == flights_.end()) {
        flight = std::make_shared<SingleFlightEntry>();
        flights_[key] = flight;
      } else {
        flight = fit->second;
      }
    }

    std::lock_guard<std::mutex> key_lock(flight->mtx);
    if (flight->done) {
      return std::static_pointer_cast<T>(flight->result);
    }

    auto created = factory();
    flight->result = created;
    flight->done = true;

    {
      std::lock_guard<std::mutex> lock(resource_mutex_);
      if (created) {
        resources_[key] = created;
      }
      flights_.erase(key);
    }
    return created;
  }

 private:
  struct SingleFlightEntry {
    std::mutex mtx;
    std::shared_ptr<void> result;
    bool done = false;
  };

  ModelManager model_manager_;
  mutable std::mutex resource_mutex_;
  std::unordered_map<std::string, std::shared_ptr<void>> resources_;
  std::unordered_map<std::string, std::shared_ptr<SingleFlightEntry>> flights_;
  RuntimeOptions runtime_options_;
};

}  // namespace llm_edgeflow
