#pragma once

#include <any>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief 句柄级运行时配置与资源参数
 */
struct RuntimeOptions {
  std::string config_file_path;
  std::string model_root_dir;
  int device_id = -1;  // -1 表示未指定/默认，>=0 表示物理设备 ID
  bool has_device_id = false;
  int biz_type = 0;
  std::string biz_name;
  std::string business_name;
  std::string chip_type = "UNKNOWN";
  int platform_max_batch = 1;
  uint32_t depth_num = 1;
};

/**
 * @brief 单句柄持有的模型实例资源池
 */
class ModelManager {
 public:
  bool RegisterModel(const std::string& model_id,
                     std::shared_ptr<IModelEngine> engine,
                     std::string revision = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (model_id.empty() || !engine) {
      return false;
    }
    if (models_.find(model_id) != models_.end()) {
      return false;
    }
    if (revision.empty()) {
      revision = std::to_string(reinterpret_cast<uintptr_t>(engine.get()));
    }
    models_[model_id] = std::move(engine);
    revisions_[model_id] = std::move(revision);
    return true;
  }

  template <typename T>
  std::shared_ptr<T> GetModel(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = models_.find(model_id);
    if (it == models_.end()) return nullptr;
    return std::dynamic_pointer_cast<T>(it->second);
  }

  bool HasModel(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_.find(model_id) != models_.end();
  }

  std::string GetModelRevision(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = revisions_.find(model_id);
    return it == revisions_.end() ? std::string() : it->second;
  }

  bool UpdateModelRevision(const std::string& model_id, std::string revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (revision.empty() || models_.find(model_id) == models_.end()) {
      return false;
    }
    revisions_[model_id] = std::move(revision);
    return true;
  }

  std::unordered_map<std::string, std::shared_ptr<IModelEngine>> GetAllModels()
      const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<IModelEngine>> models_;
  std::unordered_map<std::string, std::string> revisions_;
};

/**
 * @brief 句柄级会话上下文 (SessionContext)
 *
 * 生命周期与算法句柄绑定 (从 Alg_Create 到 Alg_Destroy)。
 * 负责存放：
 * 1. 句柄加载的多个模型实例 (ModelManager)
 * 2. 句柄级全局资源 (共享词典、预分配缓存、全局配置等)
 * 3. 句柄运行时参数 (RuntimeOptions: model_root_dir, device_id, chip_type 等)
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

}  // namespace alg_framework
