#pragma once

#include <any>
#include <memory>
#include <string>
#include <unordered_map>

#include "engine/engine_interface.h"

namespace alg_framework {

/**
 * @brief 单句柄持有的模型实例资源池
 */
class ModelManager {
 public:
  void RegisterModel(const std::string& model_id,
                     std::shared_ptr<IModelEngine> engine) {
    models_[model_id] = engine;
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
 */
class SessionContext {
 public:
  SessionContext() = default;
  ~SessionContext() = default;

  ModelManager& GetModelManager() { return model_manager_; }
  const ModelManager& GetModelManager() const { return model_manager_; }

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
};

}  // namespace alg_framework
