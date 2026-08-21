#pragma once

#include <any>
#include <memory>
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
};

/**
 * @brief 单句柄持有的模型实例资源池
 */
class ModelManager {
 public:
  bool RegisterModel(const std::string& model_id,
                     std::shared_ptr<IModelEngine> engine) {
    if (model_id.empty() || !engine) {
      return false;
    }
    if (models_.find(model_id) != models_.end()) {
      return false;
    }
    models_[model_id] = std::move(engine);
    return true;
  }

  template <typename T>
  std::shared_ptr<T> GetModel(const std::string& model_id) const {
    auto it = models_.find(model_id);
    if (it == models_.end()) return nullptr;
    return std::dynamic_pointer_cast<T>(it->second);
  }

  bool HasModel(const std::string& model_id) const {
    return models_.find(model_id) != models_.end();
  }

  const std::unordered_map<std::string, std::shared_ptr<IModelEngine>>&
  GetAllModels() const {
    return models_;
  }

 private:
  std::unordered_map<std::string, std::shared_ptr<IModelEngine>> models_;
};

/**
 * @brief 句柄级会话上下文 (SessionContext)
 *
 * 生命周期与算法句柄绑定 (从 Alg_Create 到 Alg_Destroy)。
 * 负责存放：
 * 1. 句柄加载的多个模型实例 (ModelManager)
 * 2. 句柄级全局资源 (共享词典、预分配缓存、全局配置等)
 * 3. 句柄运行时参数 (RuntimeOptions: model_root_dir, device_id 等)
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
  void SetResource(const std::string& key, std::shared_ptr<T> resource) {
    resources_[key] = resource;
  }

  template <typename T>
  std::shared_ptr<T> GetResource(const std::string& key) const {
    auto it = resources_.find(key);
    if (it == resources_.end()) return nullptr;
    return std::static_pointer_cast<T>(it->second);
  }

 private:
  ModelManager model_manager_;
  std::unordered_map<std::string, std::shared_ptr<void>> resources_;
  RuntimeOptions runtime_options_;
};

}  // namespace alg_framework
